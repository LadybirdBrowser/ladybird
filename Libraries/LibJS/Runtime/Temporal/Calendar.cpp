/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullRawPtr.h>
#include <AK/QuickSort.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/DateEquations.h>
#include <LibJS/Runtime/Temporal/Duration.h>
#include <LibJS/Runtime/Temporal/ISO8601.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>
#include <LibJS/Runtime/Temporal/PlainDateTime.h>
#include <LibJS/Runtime/Temporal/PlainMonthDay.h>
#include <LibJS/Runtime/Temporal/PlainYearMonth.h>
#include <LibJS/Runtime/Temporal/TimeZone.h>
#include <LibJS/Runtime/Temporal/ZonedDateTime.h>
#include <LibJS/Runtime/VM.h>
#include <LibUnicode/Locale.h>
#include <LibUnicode/UnicodeKeywords.h>

namespace JS::Temporal {

enum class CalendarFieldConversion {
    ToIntegerWithTruncation,
    ToMonthCode,
    ToOffsetString,
    ToPositiveIntegerWithTruncation,
    ToString,
    ToTemporalTimeZoneIdentifier,
};

// https://tc39.es/proposal-temporal/#table-temporal-calendar-fields-record-fields
#define JS_ENUMERATE_CALENDAR_FIELDS                                                                                                \
    __JS_ENUMERATE(CalendarField::Era, era, vm.names.era, CalendarFieldConversion::ToString)                                        \
    __JS_ENUMERATE(CalendarField::EraYear, era_year, vm.names.eraYear, CalendarFieldConversion::ToIntegerWithTruncation)            \
    __JS_ENUMERATE(CalendarField::Year, year, vm.names.year, CalendarFieldConversion::ToIntegerWithTruncation)                      \
    __JS_ENUMERATE(CalendarField::Month, month, vm.names.month, CalendarFieldConversion::ToPositiveIntegerWithTruncation)           \
    __JS_ENUMERATE(CalendarField::MonthCode, month_code, vm.names.monthCode, CalendarFieldConversion::ToMonthCode)                  \
    __JS_ENUMERATE(CalendarField::Day, day, vm.names.day, CalendarFieldConversion::ToPositiveIntegerWithTruncation)                 \
    __JS_ENUMERATE(CalendarField::Hour, hour, vm.names.hour, CalendarFieldConversion::ToIntegerWithTruncation)                      \
    __JS_ENUMERATE(CalendarField::Minute, minute, vm.names.minute, CalendarFieldConversion::ToIntegerWithTruncation)                \
    __JS_ENUMERATE(CalendarField::Second, second, vm.names.second, CalendarFieldConversion::ToIntegerWithTruncation)                \
    __JS_ENUMERATE(CalendarField::Millisecond, millisecond, vm.names.millisecond, CalendarFieldConversion::ToIntegerWithTruncation) \
    __JS_ENUMERATE(CalendarField::Microsecond, microsecond, vm.names.microsecond, CalendarFieldConversion::ToIntegerWithTruncation) \
    __JS_ENUMERATE(CalendarField::Nanosecond, nanosecond, vm.names.nanosecond, CalendarFieldConversion::ToIntegerWithTruncation)    \
    __JS_ENUMERATE(CalendarField::Offset, offset_string, vm.names.offset, CalendarFieldConversion::ToOffsetString)                  \
    __JS_ENUMERATE(CalendarField::TimeZone, time_zone, vm.names.timeZone, CalendarFieldConversion::ToTemporalTimeZoneIdentifier)

struct CalendarFieldData {
    CalendarField key;
    NonnullRawPtr<PropertyKey> property;
    CalendarFieldConversion conversion;
};
static Vector<CalendarFieldData> sorted_calendar_fields(VM& vm, CalendarFieldList fields)
{
    auto data_for_field = [&](auto field) -> CalendarFieldData {
        switch (field) {
#define __JS_ENUMERATE(enumeration, field_name, property_key, conversion) \
    case enumeration:                                                     \
        return { enumeration, property_key, conversion };
            JS_ENUMERATE_CALENDAR_FIELDS
#undef __JS_ENUMERATE
        }

        VERIFY_NOT_REACHED();
    };

    Vector<CalendarFieldData> result;
    result.ensure_capacity(fields.size());

    for (auto field : fields)
        result.unchecked_append(data_for_field(field));

    quick_sort(result, [](auto const& lhs, auto const& rhs) {
        return StringView { lhs.property->as_string() } < StringView { rhs.property->as_string() };
    });

    return result;
}

template<typename T>
static void set_field_value(CalendarField field, CalendarFields& fields, T&& value)
{
    switch (field) {
#define __JS_ENUMERATE(enumeration, field_name, property_key, conversion)              \
    case enumeration:                                                                  \
        if constexpr (IsAssignable<decltype(fields.field_name), RemoveCVReference<T>>) \
            fields.field_name = value;                                                 \
        return;
        JS_ENUMERATE_CALENDAR_FIELDS
#undef __JS_ENUMERATE
    }

    VERIFY_NOT_REACHED();
}

static void set_default_field_value(CalendarField field, CalendarFields& fields)
{
    CalendarFields default_ {};

    switch (field) {
#define __JS_ENUMERATE(enumeration, field_name, property_key, conversion) \
    case enumeration:                                                     \
        fields.field_name = default_.field_name;                          \
        return;
        JS_ENUMERATE_CALENDAR_FIELDS
#undef __JS_ENUMERATE
    }

    VERIFY_NOT_REACHED();
}

// 12.1.1 CanonicalizeCalendar ( id ), https://tc39.es/proposal-temporal/#sec-temporal-canonicalizecalendar
ThrowCompletionOr<String> canonicalize_calendar(VM& vm, StringView id)
{
    // 1. Let calendars be AvailableCalendars().
    auto const& calendars = available_calendars();

    // 2. If calendars does not contain the ASCII-lowercase of id, throw a RangeError exception.
    for (auto const& calendar : calendars) {
        if (calendar.equals_ignoring_ascii_case(id)) {
            // 3. Return CanonicalizeUValue("ca", id).
            return Unicode::canonicalize_unicode_extension_values("ca"sv, id);
        }
    }

    return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarIdentifier, id);
}

// 12.1.2 AvailableCalendars ( ), https://tc39.es/proposal-temporal/#sec-availablecalendars
Vector<String> const& available_calendars()
{
    // The implementation-defined abstract operation AvailableCalendars takes no arguments and returns a List of calendar
    // types. The returned List is sorted according to lexicographic code unit order, and contains unique calendar types
    // in canonical form (12.1) identifying the calendars for which the implementation provides the functionality of
    // Intl.DateTimeFormat objects, including their aliases (e.g., either both or neither of "islamicc" and
    // "islamic-civil"). The List must include "iso8601".
    return Unicode::available_calendars();
}

// 12.2.3 PrepareCalendarFields ( calendar, fields, calendarFieldNames, nonCalendarFieldNames, requiredFieldNames ), https://tc39.es/proposal-temporal/#sec-temporal-preparecalendarfields
ThrowCompletionOr<CalendarFields> prepare_calendar_fields(VM& vm, StringView calendar, Object const& fields, CalendarFieldList calendar_field_names, CalendarFieldList non_calendar_field_names, CalendarFieldListOrPartial required_field_names)
{
    // 1. Assert: If requiredFieldNames is a List, requiredFieldNames contains zero or one of each of the elements of
    //    calendarFieldNames and nonCalendarFieldNames.

    // 2. Let fieldNames be the list-concatenation of calendarFieldNames and nonCalendarFieldNames.
    Vector<CalendarField> field_names;
    field_names.append(calendar_field_names.data(), calendar_field_names.size());
    field_names.append(non_calendar_field_names.data(), non_calendar_field_names.size());

    // 3. Let extraFieldNames be CalendarExtraFields(calendar, calendarFieldNames).
    auto extra_field_names = calendar_extra_fields(calendar, calendar_field_names);

    // 4. Set fieldNames to the list-concatenation of fieldNames and extraFieldNames.
    field_names.extend(move(extra_field_names));

    // 5. Assert: fieldNames contains no duplicate elements.

    // 6. Let result be a Calendar Fields Record with all fields equal to UNSET.
    auto result = CalendarFields::unset();

    // 7. Let any be false.
    auto any = false;

    // 8. Let sortedPropertyNames be a List whose elements are the values in the Property Key column of Table 19
    //    corresponding to the elements of fieldNames, sorted according to lexicographic code unit order.
    auto sorted_property_names = sorted_calendar_fields(vm, field_names);

    // 9. For each property name property of sortedPropertyNames, do
    for (auto const& [key, property, conversion] : sorted_property_names) {
        // a. Let key be the value in the Enumeration Key column of Table 19 corresponding to the row whose Property Key value is property.

        // b. Let value be ? Get(fields, property).
        auto value = TRY(fields.get(property));

        // c. If value is not undefined, then
        if (!value.is_undefined()) {
            // i. Set any to true.
            any = true;

            // ii. Let Conversion be the Conversion value of the same row.
            switch (conversion) {
            // iii. If Conversion is TO-INTEGER-WITH-TRUNCATION, then
            case CalendarFieldConversion::ToIntegerWithTruncation:
                // 1. Set value to ? ToIntegerWithTruncation(value).
                // 2. Set value to 𝔽(value).
                set_field_value(key, result, TRY(to_integer_with_truncation(vm, value, ErrorType::TemporalInvalidCalendarFieldName, *property)));
                break;
            // iv. Else if Conversion is TO-POSITIVE-INTEGER-WITH-TRUNCATION, then
            case CalendarFieldConversion::ToPositiveIntegerWithTruncation:
                // 1. Set value to ? ToPositiveIntegerWithTruncation(value).
                // 2. Set value to 𝔽(value).
                set_field_value(key, result, TRY(to_positive_integer_with_truncation(vm, value, ErrorType::TemporalInvalidCalendarFieldName, *property)));
                break;
            // v. Else if Conversion is TO-STRING, then
            case CalendarFieldConversion::ToString:
                // 1. Set value to ? ToString(value).
                set_field_value(key, result, TRY(value.to_string(vm)));
                break;
            // vi. Else if Conversion is TO-TEMPORAL-TIME-ZONE-IDENTIFIER, then
            case CalendarFieldConversion::ToTemporalTimeZoneIdentifier:
                // 1. Set value to ? ToTemporalTimeZoneIdentifier(value).
                set_field_value(key, result, TRY(to_temporal_time_zone_identifier(vm, value)));
                break;
            // vii. Else if Conversion is TO-MONTH-CODE, then
            case CalendarFieldConversion::ToMonthCode:
                // 1. Set value to ? ToMonthCode(value).
                set_field_value(key, result, TRY(to_month_code(vm, value)));
                break;
            // viii. Else,
            case CalendarFieldConversion::ToOffsetString:
                // 1. Assert: Conversion is TO-OFFSET-STRING.
                // 2. Set value to ? ToOffsetString(value).
                set_field_value(key, result, TRY(to_offset_string(vm, value)));
                break;
            }

            // ix. Set result's field whose name is given in the Field Name column of the same row to value.
        }
        // d. Else if requiredFieldNames is a List, then
        else if (auto const* required = required_field_names.get_pointer<CalendarFieldList>()) {
            // i. If requiredFieldNames contains key, then
            if (required->contains_slow(key)) {
                // 1. Throw a TypeError exception.
                return vm.throw_completion<TypeError>(ErrorType::MissingRequiredProperty, *property);
            }

            // ii. Set result's field whose name is given in the Field Name column of the same row to the corresponding
            //     Default value of the same row.
            set_default_field_value(key, result);
        }
    }

    // 10. If requiredFieldNames is PARTIAL and any is false, then
    if (required_field_names.has<Partial>() && !any) {
        // a. Throw a TypeError exception.
        return vm.throw_completion<TypeError>(ErrorType::TemporalObjectMustBePartialTemporalObject);
    }

    // 11. Return result.
    return result;
}

// 12.2.4 CalendarFieldKeysPresent ( fields ), https://tc39.es/proposal-temporal/#sec-temporal-calendarfieldkeyspresent
Vector<CalendarField> calendar_field_keys_present(CalendarFields const& fields)
{
    // 1. Let list be « ».
    Vector<CalendarField> list;

    auto handle_field = [&](auto enumeration_key, auto const& value) {
        // a. Let value be fields' field whose name is given in the Field Name column of the row.
        // b. Let enumerationKey be the value in the Enumeration Key column of the row.
        // c. If value is not unset, append enumerationKey to list.
        if (value.has_value())
            list.append(enumeration_key);
    };

    // 2. For each row of Table 19, except the header row, do
#define __JS_ENUMERATE(enumeration, field_name, property_key, conversion) \
    handle_field(enumeration, fields.field_name);
    JS_ENUMERATE_CALENDAR_FIELDS
#undef __JS_ENUMERATE

    // 3. Return list.
    return list;
}

// 12.2.5 CalendarMergeFields ( calendar, fields, additionalFields ), https://tc39.es/proposal-temporal/#sec-temporal-calendarmergefields
CalendarFields calendar_merge_fields(StringView calendar, CalendarFields const& fields, CalendarFields const& additional_fields)
{
    // 1. Let additionalKeys be CalendarFieldKeysPresent(additionalFields).
    auto additional_keys = calendar_field_keys_present(additional_fields);

    // 2. Let overriddenKeys be CalendarFieldKeysToIgnore(calendar, additionalKeys).
    auto overridden_keys = calendar_field_keys_to_ignore(calendar, additional_keys);

    // 3. Let merged be a Calendar Fields Record with all fields set to unset.
    auto merged = CalendarFields::unset();

    // 4. Let fieldsKeys be CalendarFieldKeysPresent(fields).
    auto fields_keys = calendar_field_keys_present(fields);

    auto merge_field = [&](auto key, auto& merged_field, auto const& fields_field, auto const& additional_fields_field) {
        // a. Let key be the value in the Enumeration Key column of the row.

        // b. If fieldsKeys contains key and overriddenKeys does not contain key, then
        if (fields_keys.contains_slow(key) && !overridden_keys.contains_slow(key)) {
            // i. Let propValue be fields' field whose name is given in the Field Name column of the row.
            // ii. Set merged's field whose name is given in the Field Name column of the row to propValue.
            merged_field = fields_field;
        }

        // c. If additionalKeys contains key, then
        if (additional_keys.contains_slow(key)) {
            // i. Let propValue be additionalFields' field whose name is given in the Field Name column of the row.
            // ii. Set merged's field whose name is given in the Field Name column of the row to propValue.
            merged_field = additional_fields_field;
        }
    };

    // 5. For each row of Table 19, except the header row, do
#define __JS_ENUMERATE(enumeration, field_name, property_key, conversion) \
    merge_field(enumeration, merged.field_name, fields.field_name, additional_fields.field_name);
    JS_ENUMERATE_CALENDAR_FIELDS
#undef __JS_ENUMERATE

    // 6. Return merged.
    return merged;
}

// 12.2.6 CalendarDateAdd ( calendar, isoDate, duration, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-calendardateadd
ThrowCompletionOr<ISODate> calendar_date_add(VM& vm, StringView calendar, ISODate iso_date, DateDuration const& duration, Overflow overflow)
{
    ISODate result;

    // 1. If calendar is "iso8601", then
    // FIXME: Return an ISODate for an ISO8601 calendar for now.
    if (true || calendar == "iso8601"sv) {
        // a. Let intermediate be BalanceISOYearMonth(isoDate.[[Year]] + duration.[[Years]], isoDate.[[Month]] + duration.[[Months]]).
        auto intermediate = balance_iso_year_month(static_cast<double>(iso_date.year) + duration.years, static_cast<double>(iso_date.month) + duration.months);

        // b. Set intermediate to ? RegulateISODate(intermediate.[[Year]], intermediate.[[Month]], isoDate.[[Day]], overflow).
        auto intermediate_date = TRY(regulate_iso_date(vm, intermediate.year, intermediate.month, iso_date.day, overflow));

        // c. Let d be intermediate.[[Day]] + duration.[[Days]] + 7 × duration.[[Weeks]].
        auto day = intermediate_date.day + duration.days + (7 * duration.weeks);

        // d. Let result be BalanceISODate(intermediate.[[Year]], intermediate.[[Month]], d).
        result = balance_iso_date(intermediate_date.year, intermediate_date.month, day);
    }
    // 2. Else,
    else {
        // a. Let result be an implementation-defined ISO Date Record, or throw a RangeError exception, as described below.
    }

    // 3. If ISODateWithinLimits(result) is false, throw a RangeError exception.
    if (!iso_date_within_limits(result))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidISODate);

    // 4. Return result.
    return result;
}

// 12.2.7 CalendarDateUntil ( calendar, one, two, largestUnit ), https://tc39.es/proposal-temporal/#sec-temporal-calendardateuntil
DateDuration calendar_date_until(VM& vm, StringView calendar, ISODate one, ISODate two, Unit largest_unit)
{
    // 1. If calendar is "iso8601", then
    if (calendar == "iso8601"sv) {
        // a. Let sign be -CompareISODate(one, two).
        auto sign = compare_iso_date(one, two);
        sign *= -1;

        // b. If sign = 0, return ZeroDateDuration().
        if (sign == 0)
            return zero_date_duration(vm);

        // c. Let years be 0.
        double years = 0;

        // e. Let months be 0.
        double months = 0;

        // OPTIMIZATION: If the largestUnit is MONTH, we want to skip ahead to the correct year. If implemented in exact
        //               accordance with the spec, we could enter the second ISODateSurpasses loop below with a very large
        //               number of months to traverse.

        // d. If largestUnit is YEAR, then
        // f. If largestUnit is YEAR or largestUnit is MONTH, then
        if (largest_unit == Unit::Year || largest_unit == Unit::Month) {
            // d.i. Let candidateYears be sign.
            auto candidate_years = two.year - one.year;
            if (candidate_years != 0)
                candidate_years -= sign;

            // d.ii. Repeat, while ISODateSurpasses(sign, one.[[Year]] + candidateYears, one.[[Month]], one.[[Day]], two) is false,
            while (!iso_date_surpasses(sign, static_cast<double>(one.year) + candidate_years, one.month, one.day, two)) {
                // 1. Set years to candidateYears.
                years = candidate_years;

                // 2. Set candidateYears to candidateYears + sign.
                candidate_years += sign;
            }

            // f.i. Let candidateMonths be sign.
            double candidate_months = sign;

            // f.ii. Let intermediate be BalanceISOYearMonth(one.[[Year]] + years, one.[[Month]] + candidateMonths).
            auto intermediate = balance_iso_year_month(static_cast<double>(one.year) + years, static_cast<double>(one.month) + candidate_months);

            // f.iii. Repeat, while ISODateSurpasses(sign, intermediate.[[Year]], intermediate.[[Month]], one.[[Day]], two) is false,
            while (!iso_date_surpasses(sign, intermediate.year, intermediate.month, one.day, two)) {
                // 1. Set months to candidateMonths.
                months = candidate_months;

                // 2. Set candidateMonths to candidateMonths + sign.
                candidate_months += sign;

                // 3. Set intermediate to BalanceISOYearMonth(intermediate.[[Year]], intermediate.[[Month]] + sign).
                intermediate = balance_iso_year_month(intermediate.year, static_cast<double>(intermediate.month) + sign);
            }

            if (largest_unit == Unit::Month) {
                months += years * 12.0;
                years = 0.0;
            }
        }

        // g. Set intermediate to BalanceISOYearMonth(one.[[Year]] + years, one.[[Month]] + months).
        auto intermediate = balance_iso_year_month(static_cast<double>(one.year) + years, static_cast<double>(one.month) + months);

        // h. Let constrained be ! RegulateISODate(intermediate.[[Year]], intermediate.[[Month]], one.[[Day]], CONSTRAIN).
        auto constrained = MUST(regulate_iso_date(vm, intermediate.year, intermediate.month, one.day, Overflow::Constrain));

        // i. Let weeks be 0.
        double weeks = 0;

        // OPTIMIZATION: If the largestUnit is DAY, we do not want to enter an ISODateSurpasses loop. The loop would have
        //               us increment the intermediate ISOYearMonth one day at time, which will take an extremely long
        //               time if the difference is a large number of years. Instead, we can compute the day difference,
        //               and convert to weeks if needed.

        auto days = iso_date_to_epoch_days(two.year, two.month - 1, two.day) - iso_date_to_epoch_days(constrained.year, constrained.month - 1, constrained.day);

        if (largest_unit == Unit::Week) {
            weeks = trunc(days / 7.0);
            days = fmod(days, 7.0);
        }

        // o. Return ! CreateDateDurationRecord(years, months, weeks, days).
        return MUST(create_date_duration_record(vm, years, months, weeks, days));
    }

    // 2. Return an implementation-defined Date Duration Record as described above.
    // FIXME: Return a DateDuration for an ISO8601 calendar for now.
    return calendar_date_until(vm, "iso8601"sv, one, two, largest_unit);
}

// 12.2.8 ToTemporalCalendarIdentifier ( temporalCalendarLike ), https://tc39.es/proposal-temporal/#sec-temporal-totemporalcalendaridentifier
ThrowCompletionOr<String> to_temporal_calendar_identifier(VM& vm, Value temporal_calendar_like)
{
    // 1. If temporalCalendarLike is an Object, then
    if (temporal_calendar_like.is_object()) {
        auto const& temporal_calendar_object = temporal_calendar_like.as_object();

        // a. If temporalCalendarLike has an [[InitializedTemporalDate]], [[InitializedTemporalDateTime]],
        //    [[InitializedTemporalMonthDay]], [[InitializedTemporalYearMonth]], or [[InitializedTemporalZonedDateTime]]
        //    internal slot, then
        //     i. Return temporalCalendarLike.[[Calendar]].
        if (is<PlainDate>(temporal_calendar_object))
            return static_cast<PlainDate const&>(temporal_calendar_object).calendar();
        if (is<PlainDateTime>(temporal_calendar_object))
            return static_cast<PlainDateTime const&>(temporal_calendar_object).calendar();
        if (is<PlainMonthDay>(temporal_calendar_object))
            return static_cast<PlainMonthDay const&>(temporal_calendar_object).calendar();
        if (is<PlainYearMonth>(temporal_calendar_object))
            return static_cast<PlainYearMonth const&>(temporal_calendar_object).calendar();
        if (is<ZonedDateTime>(temporal_calendar_object))
            return static_cast<ZonedDateTime const&>(temporal_calendar_object).calendar();
    }

    // 2. If temporalCalendarLike is not a String, throw a TypeError exception.
    if (!temporal_calendar_like.is_string())
        return vm.throw_completion<TypeError>(ErrorType::TemporalInvalidCalendar);

    // 3. Let identifier be ? ParseTemporalCalendarString(temporalCalendarLike).
    auto identifier = TRY(parse_temporal_calendar_string(vm, temporal_calendar_like.as_string().utf8_string()));

    // 4. Return ? CanonicalizeCalendar(identifier).
    return TRY(canonicalize_calendar(vm, identifier));
}

// 12.2.9 GetTemporalCalendarIdentifierWithISODefault ( item ), https://tc39.es/proposal-temporal/#sec-temporal-gettemporalcalendarslotvaluewithisodefault
ThrowCompletionOr<String> get_temporal_calendar_identifier_with_iso_default(VM& vm, Object const& item)
{
    // 1. If item has an [[InitializedTemporalDate]], [[InitializedTemporalDateTime]], [[InitializedTemporalMonthDay]],
    //    [[InitializedTemporalYearMonth]], or [[InitializedTemporalZonedDateTime]] internal slot, then
    //     a. Return item.[[Calendar]].
    if (is<PlainDate>(item))
        return static_cast<PlainDate const&>(item).calendar();
    if (is<PlainDateTime>(item))
        return static_cast<PlainDateTime const&>(item).calendar();
    if (is<PlainMonthDay>(item))
        return static_cast<PlainMonthDay const&>(item).calendar();
    if (is<PlainYearMonth>(item))
        return static_cast<PlainYearMonth const&>(item).calendar();
    if (is<ZonedDateTime>(item))
        return static_cast<PlainYearMonth const&>(item).calendar();

    // 2. Let calendarLike be ? Get(item, "calendar").
    auto calendar_like = TRY(item.get(vm.names.calendar));

    // 3. If calendarLike is undefined, then
    if (calendar_like.is_undefined()) {
        // a. Return "iso8601".
        return "iso8601"_string;
    }

    // 4. Return ? ToTemporalCalendarIdentifier(calendarLike).
    return TRY(to_temporal_calendar_identifier(vm, calendar_like));
}

// 12.2.10 CalendarDateFromFields ( calendar, fields, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-calendardatefromfields
ThrowCompletionOr<ISODate> calendar_date_from_fields(VM& vm, StringView calendar, CalendarFields& fields, Overflow overflow)
{
    // 1. Perform ? CalendarResolveFields(calendar, fields, DATE).
    TRY(calendar_resolve_fields(vm, calendar, fields, DateType::Date));

    // 2. Let result be ? CalendarDateToISO(calendar, fields, overflow).
    auto result = TRY(calendar_date_to_iso(vm, calendar, fields, overflow));

    // 3. If ISODateWithinLimits(result) is false, throw a RangeError exception.
    if (!iso_date_within_limits(result))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidISODate);

    // 4. Return result.
    return result;
}

// 12.2.11 CalendarYearMonthFromFields ( calendar, fields, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-calendaryearmonthfromfields
ThrowCompletionOr<ISODate> calendar_year_month_from_fields(VM& vm, StringView calendar, CalendarFields& fields, Overflow overflow)
{
    // 1. Perform ? CalendarResolveFields(calendar, fields, YEAR-MONTH).
    TRY(calendar_resolve_fields(vm, calendar, fields, DateType::YearMonth));

    // FIXME: 2. Let firstDayIndex be the 1-based index of the first day of the month described by fields (i.e., 1 unless the
    //           month's first day is skipped by this calendar.)
    static auto constexpr first_day_index = 1;

    // 3. Set fields.[[Day]] to firstDayIndex.
    fields.day = first_day_index;

    // 4. Let result be ? CalendarDateToISO(calendar, fields, overflow).
    auto result = TRY(calendar_date_to_iso(vm, calendar, fields, overflow));

    // 5. If ISOYearMonthWithinLimits(result) is false, throw a RangeError exception.
    if (!iso_year_month_within_limits(result))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidISODate);

    // 6. Return result.
    return result;
}

// 12.2.12 CalendarMonthDayFromFields ( calendar, fields, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-calendarmonthdayfromfields
ThrowCompletionOr<ISODate> calendar_month_day_from_fields(VM& vm, StringView calendar, CalendarFields& fields, Overflow overflow)
{
    // 1. Perform ? CalendarResolveFields(calendar, fields, MONTH-DAY).
    TRY(calendar_resolve_fields(vm, calendar, fields, DateType::MonthDay));

    // 2. Let result be ? CalendarMonthDayToISOReferenceDate(calendar, fields, overflow).
    auto result = TRY(calendar_month_day_to_iso_reference_date(vm, calendar, fields, overflow));

    // 3. If ISODateWithinLimits(result) is false, throw a RangeError exception.
    if (!iso_date_within_limits(result))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidISODate);

    // 4. Return result.
    return result;
}

// 12.2.13 FormatCalendarAnnotation ( id, showCalendar ), https://tc39.es/proposal-temporal/#sec-temporal-formatcalendarannotation
String format_calendar_annotation(StringView id, ShowCalendar show_calendar)
{
    // 1. If showCalendar is NEVER, return the empty String.
    if (show_calendar == ShowCalendar::Never)
        return String {};

    // 2. If showCalendar is AUTO and id is "iso8601", return the empty String.
    if (show_calendar == ShowCalendar::Auto && id == "iso8601"sv)
        return String {};

    // 3. If showCalendar is CRITICAL, let flag be "!"; else, let flag be the empty String.
    auto flag = show_calendar == ShowCalendar::Critical ? "!"sv : ""sv;

    // 4. Return the string-concatenation of "[", flag, "u-ca=", id, and "]".
    return MUST(String::formatted("[{}u-ca={}]", flag, id));
}

// 12.2.14 CalendarEquals ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal-calendarequals
bool calendar_equals(StringView one, StringView two)
{
    // 1. If CanonicalizeUValue("ca", one) is CanonicalizeUValue("ca", two), return true.
    // 2. Return false.
    return Unicode::canonicalize_unicode_extension_values("ca"sv, one)
        == Unicode::canonicalize_unicode_extension_values("ca"sv, two);
}

// 12.2.15 ISODaysInMonth ( year, month ), https://tc39.es/proposal-temporal/#sec-temporal-isodaysinmonth
u8 iso_days_in_month(double year, double month)
{
    // 1. If month is 1, 3, 5, 7, 8, 10, or 12, return 31.
    if (month == 1 || month == 3 || month == 5 || month == 7 || month == 8 || month == 10 || month == 12)
        return 31;

    // 2. If month is 4, 6, 9, or 11, return 30.
    if (month == 4 || month == 6 || month == 9 || month == 11)
        return 30;

    // 3. Assert: month is 2.
    VERIFY(month == 2);

    // 4. Return 28 + MathematicalInLeapYear(EpochTimeForYear(year)).
    return 28 + mathematical_in_leap_year(epoch_time_for_year(year));
}

// 12.2.16 ISOWeekOfYear ( isoDate ), https://tc39.es/proposal-temporal/#sec-temporal-isoweekofyear
YearWeek iso_week_of_year(ISODate iso_date)
{
    // 1. Let year be isoDate.[[Year]].
    auto year = iso_date.year;

    // 2. Let wednesday be 3.
    static constexpr auto wednesday = 3;

    // 3. Let thursday be 4.
    static constexpr auto thursday = 4;

    // 4. Let friday be 5.
    static constexpr auto friday = 5;

    // 5. Let saturday be 6.
    static constexpr auto saturday = 6;

    // 6. Let daysInWeek be 7.
    static constexpr auto days_in_week = 7;

    // 7. Let maxWeekNumber be 53.
    static constexpr auto max_week_number = 53;

    // 8. Let dayOfYear be ISODayOfYear(isoDate).
    auto day_of_year = iso_day_of_year(iso_date);

    // 9. Let dayOfWeek be ISODayOfWeek(isoDate).
    auto day_of_week = iso_day_of_week(iso_date);

    // 10. Let week be floor((dayOfYear + daysInWeek - dayOfWeek + wednesday) / daysInWeek).
    auto week = floor(static_cast<double>(day_of_year + days_in_week - day_of_week + wednesday) / static_cast<double>(days_in_week));

    // 11. If week < 1, then
    if (week < 1) {
        // a. NOTE: This is the last week of the previous year.

        // b. Let jan1st be CreateISODateRecord(year, 1, 1).
        auto jan1st = create_iso_date_record(year, 1, 1);

        // c. Let dayOfJan1st be ISODayOfWeek(jan1st).
        auto day_of_jan1st = iso_day_of_week(jan1st);

        // d. If dayOfJan1st = friday, then
        if (day_of_jan1st == friday) {
            // i. Return Year-Week Record { [[Week]]: maxWeekNumber, [[Year]]: year - 1 }.
            return { .week = max_week_number, .year = year - 1 };
        }

        // e. If dayOfJan1st = saturday, and MathematicalInLeapYear(EpochTimeForYear(year - 1)) = 1, then
        if (day_of_jan1st == saturday && mathematical_in_leap_year(epoch_time_for_year(year - 1)) == 1) {
            // i. Return Year-Week Record { [[Week]]: maxWeekNumber. [[Year]]: year - 1 }.
            return { .week = max_week_number, .year = year - 1 };
        }

        // f. Return Year-Week Record { [[Week]]: maxWeekNumber - 1, [[Year]]: year - 1 }.
        return { .week = max_week_number - 1, .year = year - 1 };
    }

    // 12. If week = maxWeekNumber, then
    if (week == max_week_number) {
        // a. Let daysInYear be MathematicalDaysInYear(year).
        auto days_in_year = mathematical_days_in_year(year);

        // b. Let daysLaterInYear be daysInYear - dayOfYear.
        auto days_later_in_year = days_in_year - day_of_year;

        // c. Let daysAfterThursday be thursday - dayOfWeek.
        auto days_after_thursday = thursday - day_of_week;

        // d. If daysLaterInYear < daysAfterThursday, then
        if (days_later_in_year < days_after_thursday) {
            // i. Return Year-Week Record { [[Week]]: 1, [[Year]]: year + 1 }.
            return { .week = 1, .year = year + 1 };
        }
    }

    // 13. Return Year-Week Record { [[Week]]: week, [[Year]]: year }.
    return { .week = week, .year = year };
}

// 12.2.17 ISODayOfYear ( isoDate ), https://tc39.es/proposal-temporal/#sec-temporal-isodayofyear
u16 iso_day_of_year(ISODate iso_date)
{
    // 1. Let epochDays be ISODateToEpochDays(isoDate.[[Year]], isoDate.[[Month]] - 1, isoDate.[[Day]]).
    auto epoch_days = iso_date_to_epoch_days(iso_date.year, iso_date.month - 1, iso_date.day);

    // 2. Return EpochTimeToDayInYear(EpochDaysToEpochMs(epochDays, 0)) + 1.
    return epoch_time_to_day_in_year(epoch_days_to_epoch_ms(epoch_days, 0)) + 1;
}

// 12.2.18 ISODayOfWeek ( isoDate ), https://tc39.es/proposal-temporal/#sec-temporal-isodayofweek
u8 iso_day_of_week(ISODate iso_date)
{
    // 1. Let epochDays be ISODateToEpochDays(isoDate.[[Year]], isoDate.[[Month]] - 1, isoDate.[[Day]]).
    auto epoch_days = iso_date_to_epoch_days(iso_date.year, iso_date.month - 1, iso_date.day);

    // 2. Let dayOfWeek be EpochTimeToWeekDay(EpochDaysToEpochMs(epochDays, 0)).
    auto day_of_week = epoch_time_to_week_day(epoch_days_to_epoch_ms(epoch_days, 0));

    // 3. If dayOfWeek = 0, return 7.
    if (day_of_week == 0)
        return 7;

    // 4. Return dayOfWeek.
    return day_of_week;
}

// 12.2.19 CalendarDateToISO ( calendar, fields, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-calendardatetoiso
ThrowCompletionOr<ISODate> calendar_date_to_iso(VM& vm, StringView calendar, CalendarFields const& fields, Overflow overflow)
{
    // 1. If calendar is "iso8601", then
    if (calendar == "iso8601"sv) {
        // a. Assert: fields.[[Year]], fields.[[Month]], and fields.[[Day]] are not UNSET.
        VERIFY(fields.year.has_value());
        VERIFY(fields.month.has_value());
        VERIFY(fields.day.has_value());

        // b. Return ? RegulateISODate(fields.[[Year]], fields.[[Month]], fields.[[Day]], overflow).
        return TRY(regulate_iso_date(vm, *fields.year, *fields.month, *fields.day, overflow));
    }

    // 2. Return an implementation-defined ISO Date Record, or throw a RangeError exception, as described below.
    // FIXME: Create an ISODateRecord based on an ISO8601 calendar for now. See also: CalendarResolveFields.
    return calendar_date_to_iso(vm, "iso8601"sv, fields, overflow);
}

// 12.2.20 CalendarMonthDayToISOReferenceDate ( calendar, fields, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-calendarmonthdaytoisoreferencedate
ThrowCompletionOr<ISODate> calendar_month_day_to_iso_reference_date(VM& vm, StringView calendar, CalendarFields const& fields, Overflow overflow)
{
    // 1. If calendar is "iso8601", then
    if (calendar == "iso8601"sv) {
        // a. Assert: fields.[[Month]] and fields.[[Day]] are not UNSET.
        VERIFY(fields.month.has_value());
        VERIFY(fields.day.has_value());

        // b. Let referenceISOYear be 1972 (the first ISO 8601 leap year after the epoch).
        static constexpr i32 reference_iso_year = 1972;

        // c. If fields.[[Year]] is UNSET, let year be referenceISOYear; else let year be fields.[[Year]].
        auto year = !fields.year.has_value() ? reference_iso_year : *fields.year;

        // d. Let result be ? RegulateISODate(year, fields.[[Month]], fields.[[Day]], overflow).
        auto result = TRY(regulate_iso_date(vm, year, *fields.month, *fields.day, overflow));

        // e. Return CreateISODateRecord(referenceISOYear, result.[[Month]], result.[[Day]]).
        return create_iso_date_record(reference_iso_year, result.month, result.day);
    }

    // 2. Return an implementation-defined ISO Date Record, or throw a RangeError exception, as described below.
    // FIXME: Create an ISODateRecord based on an ISO8601 calendar for now. See also: CalendarResolveFields.
    return calendar_month_day_to_iso_reference_date(vm, "iso8601"sv, fields, overflow);
}

// 12.2.21 CalendarISOToDate ( calendar, isoDate ), https://tc39.es/proposal-temporal/#sec-temporal-calendarisotodate
CalendarDate calendar_iso_to_date(StringView calendar, ISODate iso_date)
{
    // 1. If calendar is "iso8601", then
    if (calendar == "iso8601"sv) {
        // a. Let monthNumberPart be ToZeroPaddedDecimalString(isoDate.[[Month]], 2).
        // b. Let monthCode be the string-concatenation of "M" and monthNumberPart.
        auto month_code = MUST(String::formatted("M{:02}", iso_date.month));

        // c. If MathematicalInLeapYear(EpochTimeForYear(isoDate.[[Year]])) = 1, let inLeapYear be true; else let inLeapYear be false.
        auto in_leap_year = mathematical_in_leap_year(epoch_time_for_year(iso_date.year)) == 1;

        // d. Return Calendar Date Record { [[Era]]: undefined, [[EraYear]]: undefined, [[Year]]: isoDate.[[Year]],
        //    [[Month]]: isoDate.[[Month]], [[MonthCode]]: monthCode, [[Day]]: isoDate.[[Day]], [[DayOfWeek]]: ISODayOfWeek(isoDate),
        //    [[DayOfYear]]: ISODayOfYear(isoDate), [[WeekOfYear]]: ISOWeekOfYear(isoDate), [[DaysInWeek]]: 7,
        //    [[DaysInMonth]]: ISODaysInMonth(isoDate.[[Year]], isoDate.[[Month]]), [[DaysInYear]]: MathematicalDaysInYear(isoDate.[[Year]]),
        //    [[MonthsInYear]]: 12, [[InLeapYear]]: inLeapYear }.
        return CalendarDate {
            .era = {},
            .era_year = {},
            .year = iso_date.year,
            .month = iso_date.month,
            .month_code = move(month_code),
            .day = iso_date.day,
            .day_of_week = iso_day_of_week(iso_date),
            .day_of_year = iso_day_of_year(iso_date),
            .week_of_year = iso_week_of_year(iso_date),
            .days_in_week = 7,
            .days_in_month = iso_days_in_month(iso_date.year, iso_date.month),
            .days_in_year = mathematical_days_in_year(iso_date.year),
            .months_in_year = 12,
            .in_leap_year = in_leap_year,
        };
    }

    // 2. Return an implementation-defined Calendar Date Record with fields as described in Table 18.
    // FIXME: Return an ISO8601 calendar date for now.
    return calendar_iso_to_date("iso8601"sv, iso_date);
}

// 12.2.22 CalendarExtraFields ( calendar, fields ), https://tc39.es/proposal-temporal/#sec-temporal-calendarextrafields
Vector<CalendarField> calendar_extra_fields(StringView calendar, CalendarFieldList)
{
    // 1. If calendar is "iso8601", return an empty List.
    if (calendar == "iso8601"sv)
        return {};

    // FIXME: 2. Return an implementation-defined List as described above.
    return {};
}

// 12.2.23 CalendarFieldKeysToIgnore ( calendar, keys ), https://tc39.es/proposal-temporal/#sec-temporal-calendarfieldkeystoignore
Vector<CalendarField> calendar_field_keys_to_ignore(StringView calendar, ReadonlySpan<CalendarField> keys)
{
    // 1. If calendar is "iso8601", then
    if (calendar == "iso8601"sv) {
        // a. Let ignoredKeys be an empty List.
        Vector<CalendarField> ignored_keys;

        // b. For each element key of keys, do
        for (auto key : keys) {
            // i. Append key to ignoredKeys.
            ignored_keys.append(key);

            // ii. If key is MONTH, append MONTH-CODE to ignoredKeys.
            if (key == CalendarField::Month)
                ignored_keys.append(CalendarField::MonthCode);
            // iii. Else if key is MONTH-CODE, append MONTH to ignoredKeys.
            else if (key == CalendarField::MonthCode)
                ignored_keys.append(CalendarField::Month);
        }

        // c. NOTE: While ignoredKeys can have duplicate elements, this is not intended to be meaningful. This specification
        //    only checks whether particular keys are or are not members of the list.

        // d. Return ignoredKeys.
        return ignored_keys;
    }

    // 2. Return an implementation-defined List as described below.
    // FIXME: Return keys for an ISO8601 calendar for now.
    return calendar_field_keys_to_ignore("iso8601"sv, keys);
}

// 12.2.24 CalendarResolveFields ( calendar, fields, type ), https://tc39.es/proposal-temporal/#sec-temporal-calendarresolvefields
ThrowCompletionOr<void> calendar_resolve_fields(VM& vm, StringView calendar, CalendarFields& fields, DateType type)
{
    // 1. If calendar is "iso8601", then
    if (calendar == "iso8601"sv) {
        // a. If type is DATE or YEAR-MONTH and fields.[[Year]] is UNSET, throw a TypeError exception.
        if ((type == DateType::Date || type == DateType::YearMonth) && !fields.year.has_value())
            return vm.throw_completion<TypeError>(ErrorType::MissingRequiredProperty, "year"sv);

        // b. If type is DATE or MONTH-DAY and fields.[[Day]] is UNSET, throw a TypeError exception.
        if ((type == DateType::Date || type == DateType::MonthDay) && !fields.day.has_value())
            return vm.throw_completion<TypeError>(ErrorType::MissingRequiredProperty, "day"sv);

        // c. Let month be fields.[[Month]].
        auto const& month = fields.month;

        // d. Let monthCode be fields.[[MonthCode]].
        auto const& month_code = fields.month_code;

        // e. If monthCode is UNSET, then
        if (!month_code.has_value()) {
            // i. If month is UNSET, throw a TypeError exception.
            if (!month.has_value())
                return vm.throw_completion<TypeError>(ErrorType::MissingRequiredProperty, "month"sv);

            // ii. Return UNUSED.
            return {};
        }

        // f. Assert: monthCode is a String.
        VERIFY(month_code.has_value());

        // g. NOTE: The ISO 8601 calendar does not include leap months.
        // h. If the length of monthCode is not 3, throw a RangeError exception.
        if (month_code->byte_count() != 3)
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFieldName, "monthCode"sv);

        // i. If the first code unit of monthCode is not 0x004D (LATIN CAPITAL LETTER M), throw a RangeError exception.
        if (month_code->bytes_as_string_view()[0] != 'M')
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFieldName, "monthCode"sv);

        // j. Let monthCodeDigits be the substring of monthCode from 1.
        auto month_code_digits = month_code->bytes_as_string_view().substring_view(1);

        // k. If ParseText(StringToCodePoints(monthCodeDigits), DateMonth) is a List of errors, throw a RangeError exception.
        if (!parse_iso8601(Production::DateMonth, month_code_digits).has_value())
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFieldName, "monthCode"sv);

        // l. Let monthCodeInteger be ℝ(StringToNumber(monthCodeDigits)).
        auto month_code_integer = month_code_digits.to_number<u8>().value();

        // m. If month is not UNSET and month ≠ monthCodeInteger, throw a RangeError exception.
        if (month.has_value() && month != month_code_integer)
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFieldName, "month"sv);

        // n. Set fields.[[Month]] to monthCodeInteger.
        fields.month = month_code_integer;
    }
    // 2. Else,
    else {
        // a. Perform implementation-defined processing to mutate fields, or throw a TypeError or RangeError exception, as described below.
        // FIXME: Resolve fields as an ISO8601 calendar for now. See also: CalendarMonthDayToISOReferenceDate.
        TRY(calendar_resolve_fields(vm, "iso8601"sv, fields, type));
    }

    // 3. Return UNUSED.
    return {};
}

}
