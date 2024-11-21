/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Intrinsics.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/PlainYearMonth.h>
#include <LibJS/Runtime/Temporal/PlainYearMonthConstructor.h>
#include <LibJS/Runtime/VM.h>

namespace JS::Temporal {

GC_DEFINE_ALLOCATOR(PlainYearMonth);

// 9 Temporal.PlainYearMonth Objects, https://tc39.es/proposal-temporal/#sec-temporal-plainyearmonth-objects
PlainYearMonth::PlainYearMonth(ISODate iso_date, String calendar, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_iso_date(iso_date)
    , m_calendar(move(calendar))
{
}

// 9.5.2 ToTemporalYearMonth ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal-totemporalyearmonth
ThrowCompletionOr<GC::Ref<PlainYearMonth>> to_temporal_year_month(VM& vm, Value item, Value options)
{
    // 1. If options is not present, set options to undefined.

    // 2. If item is an Object, then
    if (item.is_object()) {
        auto const& object = item.as_object();

        // a. If item has an [[InitializedTemporalYearMonth]] internal slot, then
        if (is<PlainYearMonth>(object)) {
            auto const& plain_year_month = static_cast<PlainYearMonth const&>(object);

            // i. Let resolvedOptions be ? GetOptionsObject(options).
            auto resolved_options = TRY(get_options_object(vm, options));

            // ii. Perform ? GetTemporalOverflowOption(resolvedOptions).
            TRY(get_temporal_overflow_option(vm, resolved_options));

            // iii. Return ! CreateTemporalYearMonth(item.[[ISODate]], item.[[Calendar]]).
            return MUST(create_temporal_year_month(vm, plain_year_month.iso_date(), plain_year_month.calendar()));
        }

        // b. Let calendar be ? GetTemporalCalendarIdentifierWithISODefault(item).
        auto calendar = TRY(get_temporal_calendar_identifier_with_iso_default(vm, object));

        // c. Let fields be ? PrepareCalendarFields(calendar, item, « YEAR, MONTH, MONTH-CODE », «», «»).
        auto fields = TRY(prepare_calendar_fields(vm, calendar, object, { { CalendarField::Year, CalendarField::Month, CalendarField::MonthCode } }, {}, CalendarFieldList {}));

        // d. Let resolvedOptions be ? GetOptionsObject(options).
        auto resolved_options = TRY(get_options_object(vm, options));

        // e. Let overflow be ? GetTemporalOverflowOption(resolvedOptions).
        auto overflow = TRY(get_temporal_overflow_option(vm, resolved_options));

        // f. Let isoDate be ? CalendarYearMonthFromFields(calendar, fields, overflow).
        auto iso_date = TRY(calendar_year_month_from_fields(vm, calendar, move(fields), overflow));

        // g. Return ! CreateTemporalYearMonth(isoDate, calendar).
        return MUST(create_temporal_year_month(vm, iso_date, move(calendar)));
    }

    // 3. If item is not a String, throw a TypeError exception.
    if (!item.is_string())
        return vm.throw_completion<TypeError>(ErrorType::TemporalInvalidPlainYearMonth);

    // 4. Let result be ? ParseISODateTime(item, « TemporalYearMonthString »).
    auto parse_result = TRY(parse_iso_date_time(vm, item.as_string().utf8_string_view(), { { Production::TemporalYearMonthString } }));

    // 5. Let calendar be result.[[Calendar]].
    // 6. If calendar is empty, set calendar to "iso8601".
    auto calendar = parse_result.calendar.value_or("iso8601"_string);

    // 7. Set calendar to ? CanonicalizeCalendar(calendar).
    calendar = TRY(canonicalize_calendar(vm, calendar));

    // 8. Let isoDate be CreateISODateRecord(result.[[Year]], result.[[Month]], result.[[Day]]).
    auto iso_date = create_iso_date_record(*parse_result.year, parse_result.month, parse_result.day);

    // 9. Set result to ISODateToFields(calendar, isoDate, YEAR-MONTH).
    auto result = iso_date_to_fields(calendar, iso_date, DateType::YearMonth);

    // 10. Let resolvedOptions be ? GetOptionsObject(options).
    auto resolved_options = TRY(get_options_object(vm, options));

    // 11. Perform ? GetTemporalOverflowOption(resolvedOptions).
    TRY(get_temporal_overflow_option(vm, resolved_options));

    // 12. NOTE: The following operation is called with CONSTRAIN regardless of the value of overflow, in order for the
    //     calendar to store a canonical value in the [[Day]] field of the [[ISODate]] internal slot of the result.
    // 13. Set isoDate to ? CalendarYearMonthFromFields(calendar, result, CONSTRAIN).
    iso_date = TRY(calendar_year_month_from_fields(vm, calendar, result, Overflow::Constrain));

    // 14. Return ! CreateTemporalYearMonth(isoDate, calendar).
    return MUST(create_temporal_year_month(vm, iso_date, move(calendar)));
}

// 9.5.3 ISOYearMonthWithinLimits ( isoDate ), https://tc39.es/proposal-temporal/#sec-temporal-isoyearmonthwithinlimits
bool iso_year_month_within_limits(ISODate iso_date)
{
    // 1. If isoDate.[[Year]] < -271821 or isoDate.[[Year]] > 275760, then
    if (iso_date.year < -271821 || iso_date.year > 275760) {
        // a. Return false.
        return false;
    }

    // 2. If isoDate.[[Year]] = -271821 and isoDate.[[Month]] < 4, then
    if (iso_date.year == -271821 && iso_date.month < 4) {
        // a. Return false.
        return false;
    }

    // 3. If isoDate.[[Year]] = 275760 and isoDate.[[Month]] > 9, then
    if (iso_date.year == 275760 && iso_date.month > 9) {
        // a. Return false.
        return false;
    }

    // 4. Return true.
    return true;
}

// 9.5.5 CreateTemporalYearMonth ( isoDate, calendar [ , newTarget ] ), https://tc39.es/proposal-temporal/#sec-temporal-createtemporalyearmonth
ThrowCompletionOr<GC::Ref<PlainYearMonth>> create_temporal_year_month(VM& vm, ISODate iso_date, String calendar, GC::Ptr<FunctionObject> new_target)
{
    auto& realm = *vm.current_realm();

    // 1. If ISOYearMonthWithinLimits(isoDate) is false, throw a RangeError exception.
    if (!iso_year_month_within_limits(iso_date))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainYearMonth);

    // 2. If newTarget is not present, set newTarget to %Temporal.PlainYearMonth%.
    if (!new_target)
        new_target = realm.intrinsics().temporal_plain_year_month_constructor();

    // 3. Let object be ? OrdinaryCreateFromConstructor(newTarget, "%Temporal.PlainYearMonth.prototype%", « [[InitializedTemporalYearMonth]], [[ISODate]], [[Calendar]] »).
    // 4. Set object.[[ISODate]] to isoDate.
    // 5. Set object.[[Calendar]] to calendar.
    auto object = TRY(ordinary_create_from_constructor<PlainYearMonth>(vm, *new_target, &Intrinsics::temporal_plain_year_month_prototype, iso_date, move(calendar)));

    // 6. Return object.
    return object;
}

// 9.5.6 TemporalYearMonthToString ( yearMonth, showCalendar ), https://tc39.es/proposal-temporal/#sec-temporal-temporalyearmonthtostring
String temporal_year_month_to_string(PlainYearMonth const& year_month, ShowCalendar show_calendar)
{
    // 1. Let year be PadISOYear(yearMonth.[[ISODate]].[[Year]]).
    auto year = pad_iso_year(year_month.iso_date().year);

    // 2. Let month be ToZeroPaddedDecimalString(yearMonth.[[ISODate]].[[Month]], 2).
    // 3. Let result be the string-concatenation of year, the code unit 0x002D (HYPHEN-MINUS), and month.
    auto result = MUST(String::formatted("{}-{:02}", year, year_month.iso_date().month));

    // 4. If showCalendar is one of always or critical, or if yearMonth.[[Calendar]] is not "iso8601", then
    if (show_calendar == ShowCalendar::Always || show_calendar == ShowCalendar::Critical || year_month.calendar() != "iso8601"sv) {
        // a. Let day be ToZeroPaddedDecimalString(yearMonth.[[ISODate]].[[Day]], 2).
        // b. Set result to the string-concatenation of result, the code unit 0x002D (HYPHEN-MINUS), and day.
        result = MUST(String::formatted("{}-{:02}", result, year_month.iso_date().day));
    }

    // 5. Let calendarString be FormatCalendarAnnotation(yearMonth.[[Calendar]], showCalendar).
    auto calendar_string = format_calendar_annotation(year_month.calendar(), show_calendar);

    // 6. Set result to the string-concatenation of result and calendarString.
    result = MUST(String::formatted("{}{}", result, calendar_string));

    // 7. Return result.
    return result;
}

}
