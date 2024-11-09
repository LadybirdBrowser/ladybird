/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainMonthDay.h>
#include <LibJS/Runtime/Temporal/PlainMonthDayConstructor.h>
#include <LibJS/Runtime/Temporal/PlainTime.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>

namespace JS::Temporal {

JS_DEFINE_ALLOCATOR(PlainMonthDay);

// 10 Temporal.PlainMonthDay Objects, https://tc39.es/proposal-temporal/#sec-temporal-plainmonthday-objects
PlainMonthDay::PlainMonthDay(u8 iso_month, u8 iso_day, i32 iso_year, Object& calendar, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_iso_year(iso_year)
    , m_iso_month(iso_month)
    , m_iso_day(iso_day)
    , m_calendar(calendar)
{
}

void PlainMonthDay::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_calendar);
}

// 10.5.1 ToTemporalMonthDay ( item [ , options ] ), https://tc39.es/proposal-temporal/#sec-temporal-totemporalmonthday
ThrowCompletionOr<PlainMonthDay*> to_temporal_month_day(VM& vm, Value item, Object const* options)
{
    // 1. If options is not present, set options to undefined.
    // 2. Assert: Type(options) is Object or Undefined.

    // 3. Let referenceISOYear be 1972 (the first leap year after the Unix epoch).
    i32 reference_iso_year = 1972;

    // 4. If Type(item) is Object, then
    if (item.is_object()) {
        auto& item_object = item.as_object();

        // a. If item has an [[InitializedTemporalMonthDay]] internal slot, then
        if (is<PlainMonthDay>(item_object)) {
            // i. Return item.
            return static_cast<PlainMonthDay*>(&item_object);
        }

        Object* calendar = nullptr;
        bool calendar_absent;

        // b. If item has an [[InitializedTemporalDate]], [[InitializedTemporalDateTime]], [[InitializedTemporalTime]], [[InitializedTemporalYearMonth]], or [[InitializedTemporalZonedDateTime]] internal slot, then
        //      i. Let calendar be item.[[Calendar]].
        //      ii. Let calendarAbsent be false.
        if (is<PlainDate>(item_object)) {
            calendar = &static_cast<PlainDate&>(item_object).calendar();
            calendar_absent = false;
        } else if (is<PlainDateTime>(item_object)) {
            calendar = &static_cast<PlainDateTime&>(item_object).calendar();
            calendar_absent = false;
        } else if (is<PlainMonthDay>(item_object)) {
            calendar = &static_cast<PlainMonthDay&>(item_object).calendar();
            calendar_absent = false;
        } else if (is<PlainTime>(item_object)) {
            calendar = &static_cast<PlainTime&>(item_object).calendar();
            calendar_absent = false;
        } else if (is<PlainYearMonth>(item_object)) {
            calendar = &static_cast<PlainYearMonth&>(item_object).calendar();
            calendar_absent = false;
        } else if (is<ZonedDateTime>(item_object)) {
            calendar = &static_cast<ZonedDateTime&>(item_object).calendar();
            calendar_absent = false;
        } else {
            // i. Let calendarLike be ? Get(item, "calendar").
            auto calendar_like = TRY(item_object.get(vm.names.calendar));

            // ii. If calendarLike is undefined, then
            //      1. Let calendarAbsent be true.
            // iii. Else,
            //      1. Let calendarAbsent be false.
            calendar_absent = calendar_like.is_undefined();

            // iv. Let calendar be ? ToTemporalCalendarWithISODefault(calendarLike).
            calendar = TRY(to_temporal_calendar_with_iso_default(vm, calendar_like));
        }

        // d. Let fieldNames be ? CalendarFields(calendar, « "day", "month", "monthCode", "year" »).
        auto field_names = TRY(calendar_fields(vm, *calendar, { "day"sv, "month"sv, "monthCode"sv, "year"sv }));

        // e. Let fields be ? PrepareTemporalFields(item, fieldNames, «»).
        auto* fields = TRY(prepare_temporal_fields(vm, item_object, field_names, Vector<StringView> {}));

        // f. Let month be ? Get(fields, "month").
        auto month = TRY(fields->get(vm.names.month));

        // g. Let monthCode be ? Get(fields, "monthCode").
        auto month_code = TRY(fields->get(vm.names.monthCode));

        // h. Let year be ? Get(fields, "year").
        auto year = TRY(fields->get(vm.names.year));

        // i. If calendarAbsent is true, and month is not undefined, and monthCode is undefined and year is undefined, then
        if (calendar_absent && !month.is_undefined() && month_code.is_undefined() && year.is_undefined()) {
            // i. Perform ! CreateDataPropertyOrThrow(fields, "year", 𝔽(referenceISOYear)).
            MUST(fields->create_data_property_or_throw(vm.names.year, Value(reference_iso_year)));
        }

        // j. Return ? CalendarMonthDayFromFields(calendar, fields, options).
        return calendar_month_day_from_fields(vm, *calendar, *fields, options);
    }

    // 5. Perform ? ToTemporalOverflow(options).
    (void)TRY(to_temporal_overflow(vm, options));

    // 6. Let string be ? ToString(item).
    auto string = TRY(item.to_string(vm));

    // 7. Let result be ? ParseTemporalMonthDayString(string).
    auto result = TRY(parse_temporal_month_day_string(vm, string));

    // 8. Let calendar be ? ToTemporalCalendarWithISODefault(result.[[Calendar]]).
    auto* calendar = TRY(to_temporal_calendar_with_iso_default(vm, result.calendar.has_value() ? PrimitiveString::create(vm, move(*result.calendar)) : js_undefined()));

    // 9. If result.[[Year]] is undefined, then
    if (!result.year.has_value()) {
        // a. Return ? CreateTemporalMonthDay(result.[[Month]], result.[[Day]], calendar, referenceISOYear).
        return TRY(create_temporal_month_day(vm, result.month, result.day, *calendar, reference_iso_year));
    }

    // 10. Set result to ? CreateTemporalMonthDay(result.[[Month]], result.[[Day]], calendar, referenceISOYear).
    auto* plain_month_day = TRY(create_temporal_month_day(vm, result.month, result.day, *calendar, reference_iso_year));

    // 11. NOTE: The following operation is called without options, in order for the calendar to store a canonical value in the [[ISOYear]] internal slot of the result.
    // 12. Return ? CalendarMonthDayFromFields(calendar, result).
    return TRY(calendar_month_day_from_fields(vm, *calendar, *plain_month_day));
}

// 10.5.2 CreateTemporalMonthDay ( isoMonth, isoDay, calendar, referenceISOYear [ , newTarget ] ), https://tc39.es/proposal-temporal/#sec-temporal-createtemporalmonthday
ThrowCompletionOr<PlainMonthDay*> create_temporal_month_day(VM& vm, u8 iso_month, u8 iso_day, Object& calendar, i32 reference_iso_year, FunctionObject const* new_target)
{
    auto& realm = *vm.current_realm();

    // 1. Assert: isoMonth, isoDay, and referenceISOYear are integers.
    // 2. Assert: Type(calendar) is Object.

    // 3. If IsValidISODate(referenceISOYear, isoMonth, isoDay) is false, throw a RangeError exception.
    if (!is_valid_iso_date(reference_iso_year, iso_month, iso_day))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainMonthDay);

    // 4. If ISODateTimeWithinLimits(referenceISOYear, isoMonth, isoDay, 12, 0, 0, 0, 0, 0) is false, throw a RangeError exception.
    if (!iso_date_time_within_limits(reference_iso_year, iso_month, iso_day, 12, 0, 0, 0, 0, 0))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidPlainMonthDay);

    // 5. If newTarget is not present, set newTarget to %Temporal.PlainMonthDay%.
    if (!new_target)
        new_target = realm.intrinsics().temporal_plain_month_day_constructor();

    // 6. Let object be ? OrdinaryCreateFromConstructor(newTarget, "%Temporal.PlainMonthDay.prototype%", « [[InitializedTemporalMonthDay]], [[ISOMonth]], [[ISODay]], [[ISOYear]], [[Calendar]] »).
    // 7. Set object.[[ISOMonth]] to isoMonth.
    // 8. Set object.[[ISODay]] to isoDay.
    // 9. Set object.[[Calendar]] to calendar.
    // 10. Set object.[[ISOYear]] to referenceISOYear.
    auto object = TRY(ordinary_create_from_constructor<PlainMonthDay>(vm, *new_target, &Intrinsics::temporal_plain_month_day_prototype, iso_month, iso_day, reference_iso_year, calendar));

    // 11. Return object.
    return object.ptr();
}

// 10.5.3 TemporalMonthDayToString ( monthDay, showCalendar ), https://tc39.es/proposal-temporal/#sec-temporal-temporalmonthdaytostring
ThrowCompletionOr<String> temporal_month_day_to_string(VM& vm, PlainMonthDay& month_day, StringView show_calendar)
{
    // 1. Assert: Type(monthDay) is Object.
    // 2. Assert: monthDay has an [[InitializedTemporalMonthDay]] internal slot.

    // 3. Let month be ToZeroPaddedDecimalString(temporalDate.[[ISOMonth]], 2).
    // 4. Let day be ToZeroPaddedDecimalString(temporalDate.[[ISODay]], 2).
    // 5. Let result be the string-concatenation of month, the code unit 0x002D (HYPHEN-MINUS), and day.
    auto result = TRY_OR_THROW_OOM(vm, String::formatted("{:02}-{:02}", month_day.iso_month(), month_day.iso_day()));

    // 6. Let calendarID be ? ToString(monthDay.[[Calendar]]).
    auto calendar_id = TRY(Value(&month_day.calendar()).to_string(vm));

    // 7. If showCalendar is one of "always" or "critical", or if calendarID is not "iso8601", then
    if (show_calendar.is_one_of("always"sv, "critical"sv) || calendar_id != "iso8601"sv) {
        // a. Let year be ! PadISOYear(monthDay.[[ISOYear]]).
        // b. Set result to the string-concatenation of year, the code unit 0x002D (HYPHEN-MINUS), and result.
        result = TRY_OR_THROW_OOM(vm, String::formatted("{}-{}", MUST_OR_THROW_OOM(pad_iso_year(vm, month_day.iso_year())), result));
    }

    // 8. Let calendarString be ! FormatCalendarAnnotation(calendarID, showCalendar).
    auto calendar_string = MUST_OR_THROW_OOM(format_calendar_annotation(vm, calendar_id, show_calendar));

    // 9. Set result to the string-concatenation of result and calendarString.
    // 10. Return result.
    return TRY_OR_THROW_OOM(vm, String::formatted("{}{}", result, calendar_string));
}

}
