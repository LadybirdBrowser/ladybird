/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <AK/NonnullRawPtr.h>
#include <AK/QuickSort.h>
#include <LibJS/Runtime/Temporal/Calendar.h>
#include <LibJS/Runtime/Temporal/DateEquations.h>
#include <LibJS/Runtime/Temporal/Duration.h>
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

String ISO8601_CALENDAR = "iso8601"_string;

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
        return lhs.property->as_string() < rhs.property->as_string();
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

// Table 1: Calendar types described in CLDR, https://tc39.es/proposal-intl-era-monthcode/#table-calendar-types
static constexpr auto CLDR_CALENDAR_TYPES = to_array({
    "buddhist"sv,
    "chinese"sv,
    "coptic"sv,
    "dangi"sv,
    "ethioaa"sv,
    "ethiopic"sv,
    "ethiopic-amete-alem"sv,
    "gregory"sv,
    "hebrew"sv,
    "indian"sv,
    "islamic-civil"sv,
    "islamic-tbla"sv,
    "islamic-umalqura"sv,
    "islamicc"sv,
    "iso8601"sv,
    "japanese"sv,
    "persian"sv,
    "roc"sv,
});

// Table 2: Eras, https://tc39.es/proposal-intl-era-monthcode/#table-eras
struct CalendarEraData {
    enum class Kind : u8 {
        Epoch,
        Offset,
        Negative,
    };

    StringView calendar;
    StringView era;
    StringView alias;
    Optional<i32> minimum_era_year;
    Optional<i32> maximum_era_year;
    Kind kind;
    Optional<i32> offset;

    // NB: This column is not in the spec table, but is needed to handle calendars with mid-year era transitions.
    Optional<ISODate> iso_era_start;
};
static constexpr auto CALENDAR_ERA_DATA = to_array<CalendarEraData>({
    // clang-format off
    { "buddhist"sv,         "be"sv,     {},     {}, {},   CalendarEraData::Kind::Epoch,    {},    {}                   },
    { "coptic"sv,           "am"sv,     {},     {}, {},   CalendarEraData::Kind::Epoch,    {},    {}                   },
    { "ethioaa"sv,          "aa"sv,     {},     {}, {},   CalendarEraData::Kind::Epoch,    {},    {}                   },
    { "ethiopic"sv,         "am"sv,     {},     1,  {},   CalendarEraData::Kind::Epoch,    {},    {}                   },
    { "ethiopic"sv,         "aa"sv,     {},     {}, 5500, CalendarEraData::Kind::Offset,   -5499, {}                   },
    { "gregory"sv,          "ce"sv,     "ad"sv, 1,  {},   CalendarEraData::Kind::Epoch,    {},    {}                   },
    { "gregory"sv,          "bce"sv,    "bc"sv, 1,  {},   CalendarEraData::Kind::Negative, {},    {}                   },
    { "hebrew"sv,           "am"sv,     {},     {}, {},   CalendarEraData::Kind::Epoch,    {},    {}                   },
    { "indian"sv,           "shaka"sv,  {},     {}, {},   CalendarEraData::Kind::Epoch,    {},    {}                   },
    { "islamic-civil"sv,    "ah"sv,     {},     1,  {},   CalendarEraData::Kind::Epoch,    {},    {}                   },
    { "islamic-civil"sv,    "bh"sv,     {},     1,  {},   CalendarEraData::Kind::Negative, {},    {}                   },
    { "islamic-tbla"sv,     "ah"sv,     {},     1,  {},   CalendarEraData::Kind::Epoch,    {},    {}                   },
    { "islamic-tbla"sv,     "bh"sv,     {},     1,  {},   CalendarEraData::Kind::Negative, {},    {}                   },
    { "islamic-umalqura"sv, "ah"sv,     {},     1,  {},   CalendarEraData::Kind::Epoch,    {},    {}                   },
    { "islamic-umalqura"sv, "bh"sv,     {},     1,  {},   CalendarEraData::Kind::Negative, {},    {}                   },
    { "japanese"sv,         "reiwa"sv,  {},     1,  {},   CalendarEraData::Kind::Offset,   2019,  { { 2019, 5, 1 } }   },
    { "japanese"sv,         "heisei"sv, {},     1,  31,   CalendarEraData::Kind::Offset,   1989,  { { 1989, 1, 8 } }   },
    { "japanese"sv,         "showa"sv,  {},     1,  64,   CalendarEraData::Kind::Offset,   1926,  { { 1926, 12, 25 } } },
    { "japanese"sv,         "taisho"sv, {},     1,  15,   CalendarEraData::Kind::Offset,   1912,  { { 1912, 7, 30 } }  },
    { "japanese"sv,         "meiji"sv,  {},     1,  45,   CalendarEraData::Kind::Offset,   1868,  { { 1873, 1, 1 } }   },
    { "japanese"sv,         "ce"sv,     "ad"sv, 1,  1872, CalendarEraData::Kind::Epoch,    {},    {}                   },
    { "japanese"sv,         "bce"sv,    "bc"sv, 1,  {},   CalendarEraData::Kind::Negative, {},    {}                   },
    { "persian"sv,          "ap"sv,     {},     {}, {},   CalendarEraData::Kind::Epoch,    {},    {}                   },
    { "roc"sv,              "roc"sv,    {},     1,  {},   CalendarEraData::Kind::Epoch,    {},    {}                   },
    { "roc"sv,              "broc"sv,   {},     1,  {},   CalendarEraData::Kind::Negative, {},    {}                   },
    // clang-format on
});

// Table 3: Additional Month Codes in Calendars, https://tc39.es/proposal-intl-era-monthcode/#table-additional-month-codes
struct AdditionalMonthCodes {
    enum class Leap : u8 {
        SkipBackward,
        SkipForward,
    };

    StringView calendar;
    ReadonlySpan<StringView> additional_month_codes;
    Optional<Leap> leap_to_common_month_transformation;
};

static constexpr auto ALL_LEAP_MONTH_CODES = to_array({ "M01L"sv, "M02L"sv, "M03L"sv, "M04L"sv, "M05L"sv, "M06L"sv, "M07L"sv, "M08L"sv, "M09L"sv, "M10L"sv, "M11L"sv, "M12L"sv });
static constexpr auto THIRTEENTH_MONTH_CODES = to_array({ "M13"sv });
static constexpr auto HEBREW_ADAR_I_MONTH_CODES = to_array({ "M05L"sv });

static constexpr auto ADDITIONAL_MONTH_CODES = to_array<AdditionalMonthCodes>({
    { "chinese"sv, ALL_LEAP_MONTH_CODES, AdditionalMonthCodes::Leap::SkipBackward },
    { "coptic"sv, THIRTEENTH_MONTH_CODES, {} },
    { "dangi"sv, ALL_LEAP_MONTH_CODES, AdditionalMonthCodes::Leap::SkipBackward },
    { "ethioaa"sv, THIRTEENTH_MONTH_CODES, {} },
    { "ethiopic"sv, THIRTEENTH_MONTH_CODES, {} },
    { "hebrew"sv, HEBREW_ADAR_I_MONTH_CODES, AdditionalMonthCodes::Leap::SkipForward },
});

// Table 6: "chinese" and "dangi" Calendars ISO Reference Years, https://tc39.es/proposal-intl-era-monthcode/#chinese-dangi-iso-reference-years
struct ISOReferenceYears {
    StringView month_code;
    Optional<i32> days_1_to_29;
    Optional<i32> day_30;
};
static constexpr auto CHINESE_AND_DANGI_ISO_REFERENCE_YEARS = to_array<ISOReferenceYears>({
    { "M01"sv, 1972, 1970 },
    { "M01L"sv, {}, {} },
    { "M02"sv, 1972, 1972 },
    { "M02L"sv, 1947, {} },
    { "M03"sv, 1972, 0 }, // Day=30 depends on the calendar and is handled below.
    { "M03L"sv, 1966, 1955 },
    { "M04"sv, 1972, 1970 },
    { "M04L"sv, 1963, 1944 },
    { "M05"sv, 1972, 1972 },
    { "M05L"sv, 1971, 1952 },
    { "M06"sv, 1972, 1971 },
    { "M06L"sv, 1960, 1941 },
    { "M07"sv, 1972, 1972 },
    { "M07L"sv, 1968, 1938 },
    { "M08"sv, 1972, 1971 },
    { "M08L"sv, 1957, {} },
    { "M09"sv, 1972, 1972 },
    { "M09L"sv, 2014, {} },
    { "M10"sv, 1972, 1972 },
    { "M10L"sv, 1984, {} },
    { "M11"sv, 1972, 1970 },
    { "M11L"sv, 0, {} }, // The reference year for days 1-10 and days 11-29 differ and is handled below.
    { "M12"sv, 1972, 1972 },
    { "M12L"sv, {}, {} },
});

static Optional<i32> chinese_or_dangi_reference_year(String const& calendar, StringView month_code, u8 day)
{
    auto row = find_value(CHINESE_AND_DANGI_ISO_REFERENCE_YEARS, [&](auto const& row) { return row.month_code == month_code; });
    VERIFY(row.has_value());

    if (day >= 1 && day < 30) {
        if (month_code == "M11L"sv)
            return day <= 10 ? 2033 : 2034;
        return row->days_1_to_29;
    }

    if (day == 30) {
        if (month_code == "M03"sv)
            return calendar == "chinese"sv ? 1966 : 1968;
        return row->day_30;
    }

    return {};
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
// 1.1.1 AvailableCalendars ( ), https://tc39.es/proposal-intl-era-monthcode/#sup-availablecalendars
Vector<String> const& available_calendars()
{
    // The implementation-defined abstract operation AvailableCalendars takes no arguments and returns a List of calendar
    // types. The returned List is sorted according to lexicographic code unit order, and contains unique calendar types
    // in canonical form (12.1) identifying the calendars for which the implementation provides the functionality of
    // Intl.DateTimeFormat objects, including their aliases (e.g., both "islamicc" and "islamic-civil"). The List must
    // consist of the "Calendar Type" value of every row of Table 1, except the header row.
    static auto calendars = []() {
        auto calendars = Unicode::available_calendars();

        // NB: It is up in the air whether ECMA-402 and Temporal will support "islamic" and "islamic-rgsa". See:
        //     https://github.com/tc39/ecma402/issues/971
        //     https://github.com/tc39/ecma402/issues/1042
        //     https://github.com/tc39/proposal-intl-era-monthcode/issues/29
        //
        //     In the meantime, we don't include them here as they do not appear in the list of supported calendars
        //     which test262 relies on. See:
        //     https://tc39.es/proposal-intl-era-monthcode/#sec-ecma402-calendar-types
        calendars.remove_all_matching([](auto calendar) {
            return calendar.is_one_of("islamic"sv, "islamic-rgsa"sv);
        });

        for (auto calendar : CLDR_CALENDAR_TYPES) {
            if (!calendars.contains_slow(calendar))
                calendars.append(String::from_utf8_without_validation(calendar.bytes()));
        }

        quick_sort(calendars);
        return calendars;
    }();

    return calendars;
}

// https://tc39.es/proposal-temporal/#prod-MonthCode
static constexpr bool is_valid_month_code_string(StringView month_code)
{
    // MonthCode :::
    //     M00L
    //     M0 NonZeroDigit L[opt]
    //     M NonZeroDigit DecimalDigit L[opt]
    auto length = month_code.length();

    if (length != 3 && length != 4)
        return false;

    if (month_code[0] != 'M')
        return false;

    if (!is_ascii_digit(month_code[1]) || !is_ascii_digit(month_code[2]))
        return false;

    if (length == 3 && month_code[1] == '0' && month_code[2] == '0')
        return false;
    if (length == 4 && month_code[3] != 'L')
        return false;

    return true;
}

// 12.2.1 ParseMonthCode ( argument ), https://tc39.es/proposal-temporal/#sec-temporal-parsemonthcode
ThrowCompletionOr<MonthCode> parse_month_code(VM& vm, Value argument)
{
    // 1. Let monthCode be ? ToPrimitive(argument, STRING).
    auto month_code = TRY(argument.to_primitive(vm, Value::PreferredType::String));

    // 2. If monthCode is not a String, throw a TypeError exception.
    if (!month_code.is_string())
        return vm.throw_completion<TypeError>(ErrorType::NotAString, month_code);

    return parse_month_code(vm, month_code.as_string().utf8_string_view());
}

// 12.2.1 ParseMonthCode ( argument ), https://tc39.es/proposal-temporal/#sec-temporal-parsemonthcode
ThrowCompletionOr<MonthCode> parse_month_code(VM& vm, StringView month_code)
{
    // 3. If ParseText(StringToCodePoints(monthCode), MonthCode) is a List of errors, throw a RangeError exception.
    if (!is_valid_month_code_string(month_code))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidMonthCode);

    return parse_month_code(month_code);
}

// 12.2.1 ParseMonthCode ( argument ), https://tc39.es/proposal-temporal/#sec-temporal-parsemonthcode
MonthCode parse_month_code(StringView month_code)
{
    // 4. Let isLeapMonth be false.
    auto is_leap_month = false;

    // 5. If the length of monthCode = 4, then
    if (month_code.length() == 4) {
        // a. Assert: The fourth code unit of monthCode is 0x004C (LATIN CAPITAL LETTER L).
        VERIFY(month_code[3] == 'L');

        // b. Set isLeapMonth to true.
        is_leap_month = true;
    }

    // 6. Let monthCodeDigits be the substring of monthCode from 1 to 3.
    auto month_code_digits = month_code.substring_view(1, 2);

    // 7. Let monthNumber be ℝ(StringToNumber(monthCodeDigits)).
    auto month_number = month_code_digits.to_number<u8>().value();

    // 8. Return the Record { [[MonthNumber]]: monthNumber, [[IsLeapMonth]]: isLeapMonth }.
    return MonthCode { month_number, is_leap_month };
}

// 12.2.2 CreateMonthCode ( monthNumber, isLeapMonth ), https://tc39.es/proposal-temporal/#sec-temporal-createmonthcode
String create_month_code(u8 month_number, bool is_leap_month)
{
    // 1. Assert: If isLeapMonth is false, monthNumber > 0.
    if (!is_leap_month)
        VERIFY(month_number > 0);

    // 2. Let numberPart be ToZeroPaddedDecimalString(monthNumber, 2).

    // 3. If isLeapMonth is true, then
    if (is_leap_month) {
        // a. Return the string-concatenation of the code unit 0x004D (LATIN CAPITAL LETTER M), numberPart, and the
        //    code unit 0x004C (LATIN CAPITAL LETTER L).
        return MUST(String::formatted("M{:02}L", month_number));
    }

    // 4. Return the string-concatenation of the code unit 0x004D (LATIN CAPITAL LETTER M) and numberPart.
    return MUST(String::formatted("M{:02}", month_number));
}

// 12.3.3 PrepareCalendarFields ( calendar, fields, calendarFieldNames, nonCalendarFieldNames, requiredFieldNames ), https://tc39.es/proposal-temporal/#sec-temporal-preparecalendarfields
ThrowCompletionOr<CalendarFields> prepare_calendar_fields(VM& vm, String const& calendar, Object const& fields, CalendarFieldList calendar_field_names, CalendarFieldList non_calendar_field_names, CalendarFieldListOrPartial required_field_names)
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
            case CalendarFieldConversion::ToMonthCode: {
                // 1. Let parsed be ? ParseMonthCode(value).
                auto parsed = TRY(parse_month_code(vm, value));

                // 2. Set value to CreateMonthCode(parsed.[[MonthNumber]], parsed.[[IsLeapMonth]]).
                set_field_value(key, result, create_month_code(parsed.month_number, parsed.is_leap_month));

                break;
            }
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
            // i. If requiredFieldNames contains key, throw a TypeError exception.
            if (required->contains_slow(key))
                return vm.throw_completion<TypeError>(ErrorType::MissingRequiredProperty, *property);

            // ii. Set result's field whose name is given in the Field Name column of the same row to the corresponding
            //     Default value of the same row.
            set_default_field_value(key, result);
        }
    }

    // 10. If requiredFieldNames is PARTIAL and any is false, throw a TypeError exception.
    if (required_field_names.has<Partial>() && !any)
        return vm.throw_completion<TypeError>(ErrorType::TemporalObjectMustBePartialTemporalObject);

    // 11. Return result.
    return result;
}

// 12.3.4 CalendarFieldKeysPresent ( fields ), https://tc39.es/proposal-temporal/#sec-temporal-calendarfieldkeyspresent
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

// 12.3.5 CalendarMergeFields ( calendar, fields, additionalFields ), https://tc39.es/proposal-temporal/#sec-temporal-calendarmergefields
CalendarFields calendar_merge_fields(String const& calendar, CalendarFields const& fields, CalendarFields const& additional_fields)
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

// 12.3.6 NonISODateAdd ( calendar, isoDate, duration, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-nonisodateadd
// 4.1.18 NonISODateAdd ( calendar, isoDate, duration, overflow ), https://tc39.es/proposal-intl-era-monthcode/#sup-temporal-nonisodateadd
ThrowCompletionOr<ISODate> non_iso_date_add(VM& vm, String const& calendar, ISODate iso_date, DateDuration const& duration, Overflow overflow)
{
    // 1. Let parts be CalendarISOToDate(calendar, isoDate).
    auto parts = non_iso_calendar_iso_to_date(calendar, iso_date);

    // 2. Let y0 be parts.[[Year]] + duration.[[Years]].
    auto y0 = parts.year + static_cast<i32>(duration.years);

    // 3. Let m0 be MonthCodeToOrdinal(calendar, y0, ? ConstrainMonthCode(calendar, y0, parts.[[MonthCode]], overflow)).
    auto m0 = month_code_to_ordinal(calendar, y0, TRY(constrain_month_code(vm, calendar, y0, parts.month_code, overflow)));

    // 4. Let endOfMonth be BalanceNonISODate(calendar, y0, m0 + duration.[[Months]] + 1, 0).
    auto end_of_month = balance_non_iso_date(calendar, y0, static_cast<i32>(m0 + duration.months + 1), 0);

    // 5. Let baseDay be parts.[[Day]].
    auto base_day = parts.day;

    u8 regulated_day = 0;

    // 6. If baseDay ≤ endOfMonth.[[Day]], then
    if (base_day <= end_of_month.day) {
        // a. Let regulatedDay be baseDay.
        regulated_day = base_day;
    }
    // 7. Else,
    else {
        // a. If overflow is REJECT, throw a RangeError exception.
        if (overflow == Overflow::Reject)
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidISODate);

        // b. Let regulatedDay be endOfMonth.[[Day]].
        regulated_day = end_of_month.day;
    }

    // 8. Let balancedDate be BalanceNonISODate(calendar, endOfMonth.[[Year]], endOfMonth.[[Month]], regulatedDay + 7 * duration.[[Weeks]] + duration.[[Days]]).
    auto balanced_date = balance_non_iso_date(calendar, end_of_month.year, end_of_month.month, static_cast<i32>(regulated_day + (7 * duration.weeks) + duration.days));

    // 9. Let result be ? CalendarIntegersToISO(calendar, balancedDate.[[Year]], balancedDate.[[Month]], balancedDate.[[Day]]).
    auto result = TRY(calendar_integers_to_iso(vm, calendar, balanced_date.year, balanced_date.month, balanced_date.day));

    // 10. If ISODateWithinLimits(result) is false, throw a RangeError exception.
    // NB: This is handled by the caller, CalendarDateAdd.

    // 11. Return result.
    return result;
}

// 12.3.7 CalendarDateAdd ( calendar, isoDate, duration, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-calendardateadd
ThrowCompletionOr<ISODate> calendar_date_add(VM& vm, String const& calendar, ISODate iso_date, DateDuration const& duration, Overflow overflow)
{
    ISODate result;

    // 1. If calendar is "iso8601", then
    if (calendar == ISO8601_CALENDAR) {
        // a. Let intermediate be BalanceISOYearMonth(isoDate.[[Year]] + duration.[[Years]], isoDate.[[Month]] + duration.[[Months]]).
        auto intermediate = balance_iso_year_month(static_cast<double>(iso_date.year) + duration.years, static_cast<double>(iso_date.month) + duration.months);

        // b. Set intermediate to ? RegulateISODate(intermediate.[[Year]], intermediate.[[Month]], isoDate.[[Day]], overflow).
        auto intermediate_date = TRY(regulate_iso_date(vm, intermediate.year, intermediate.month, iso_date.day, overflow));

        // c. Let days be duration.[[Days]] + 7 × duration.[[Weeks]].
        auto days = duration.days + (7 * duration.weeks);

        // d. Let result be AddDaysToISODate(intermediate, days).
        result = add_days_to_iso_date(intermediate_date, days);
    }
    // 2. Else,
    else {
        // a. Let result be ? NonISODateAdd(calendar, isoDate, duration, overflow).
        result = TRY(non_iso_date_add(vm, calendar, iso_date, duration, overflow));
    }

    // 3. If ISODateWithinLimits(result) is false, throw a RangeError exception.
    if (!iso_date_within_limits(result))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidISODate);

    // 4. Return result.
    return result;
}

// 12.3.8 NonISODateUntil ( calendar, one, two, largestUnit ), https://tc39.es/proposal-temporal/#sec-temporal-nonisodateuntil
// 4.1.19 NonISODateUntil ( calendar, one, two, largestUnit ), https://tc39.es/proposal-intl-era-monthcode/#sup-temporal-nonisodateuntil
DateDuration non_iso_date_until(VM& vm, String const& calendar, ISODate one, ISODate two, Unit largest_unit)
{
    // 1. Let sign be -1 × CompareISODate(one, two).
    auto sign = compare_iso_date(one, two);
    sign *= -1;

    // 2. If sign = 0, return ZeroDateDuration().
    if (sign == 0)
        return zero_date_duration(vm);

    // OPTIMIZATION: For DAY and WEEK largest units, calendar days equal ISO days. We can compute the difference
    //               directly from ISO epoch days without any calendar arithmetic.
    if (largest_unit == Unit::Day || largest_unit == Unit::Week) {
        auto days = iso_date_to_epoch_days(two.year, two.month - 1, two.day) - iso_date_to_epoch_days(one.year, one.month - 1, one.day);
        double weeks = 0;

        if (largest_unit == Unit::Week) {
            weeks = trunc(days / 7.0);
            days = fmod(days, 7.0);
        }

        return MUST(create_date_duration_record(vm, 0, 0, weeks, days));
    }

    // OPTIMIZATION: Pre-compute calendar dates for `from` and `to` to avoid expensive redundant conversions.
    auto calendar_one = non_iso_calendar_iso_to_date(calendar, one);
    auto calendar_two = non_iso_calendar_iso_to_date(calendar, two);

    // 3. Let years be 0.
    double years = 0;

    // OPTIMIZATION: If the largestUnit is MONTH, we want to skip ahead to the correct year. If implemented in exact
    //               accordance with the spec, we could enter the second NonISODateSurpasses loop below with a very
    //               large number of months to traverse.

    // 4. If largestUnit is YEAR, then
    if (largest_unit == Unit::Year) {
        // OPTIMIZATION: Skip ahead by estimating the year difference from calendar dates to avoid a large number of
        //               iterations in the NonISODateSurpasses loop below.
        auto estimated_years = calendar_two.year - calendar_one.year;
        if (estimated_years != 0)
            estimated_years -= sign;

        // a. Let candidateYears be sign.
        double candidate_years = estimated_years ? estimated_years : static_cast<double>(sign);

        // b. Repeat, while NonISODateSurpasses(calendar, sign, one, two, candidateYears, 0, 0, 0) is false,
        while (!non_iso_date_surpasses(vm, calendar, sign, calendar_one, calendar_two, candidate_years, 0, 0, 0)) {
            // i. Set years to candidateYears.
            years = candidate_years;

            // ii. Set candidateYears to candidateYears + sign.
            candidate_years += sign;
        }
    }

    // 5. Let months be 0.
    double months = 0;

    // 6. If largestUnit is YEAR or largestUnit is MONTH, then
    if (largest_unit == Unit::Year || largest_unit == Unit::Month) {
        // a. Let candidateMonths be sign.
        double candidate_months = sign;

        // OPTIMIZATION: Skip ahead by estimating the month difference from calendar dates to avoid a large number of
        //               iterations in the NonISODateSurpasses loop below.
        if (largest_unit == Unit::Month) {
            auto estimated_months = ((calendar_two.year - calendar_one.year) * 12) + (calendar_two.month - calendar_one.month);
            if (estimated_months != 0)
                estimated_months -= sign;

            if (estimated_months != 0)
                candidate_months = estimated_months;
        }

        // b. Repeat, while NonISODateSurpasses(calendar, sign, one, two, years, candidateMonths, 0, 0) is false,
        while (!non_iso_date_surpasses(vm, calendar, sign, calendar_one, calendar_two, years, candidate_months, 0, 0)) {
            // i. Set months to candidateMonths.
            months = candidate_months;

            // ii. Set candidateMonths to candidateMonths + sign.
            candidate_months += sign;
        }
    }

    // 9. Let days be 0.
    double days = 0;

    // 10. Let candidateDays be sign.
    double candidate_days = sign;

    // 11. Repeat, while NonISODateSurpasses(calendar, sign, one, two, years, months, weeks, candidateDays) is false,
    while (!non_iso_date_surpasses(vm, calendar, sign, calendar_one, calendar_two, years, months, 0, candidate_days)) {
        // a. Set days to candidateDays.
        days = candidate_days;

        // b. Set candidateDays to candidateDays + sign.
        candidate_days += sign;
    }

    // 12. Return ! CreateDateDurationRecord(years, months, weeks, days).
    return MUST(create_date_duration_record(vm, years, months, 0, days));
}

// 12.3.9 CalendarDateUntil ( calendar, one, two, largestUnit ), https://tc39.es/proposal-temporal/#sec-temporal-calendardateuntil
DateDuration calendar_date_until(VM& vm, String const& calendar, ISODate one, ISODate two, Unit largest_unit)
{
    // 1. Let sign be CompareISODate(one, two).
    auto sign = compare_iso_date(one, two);

    // 2. If sign = 0, return ZeroDateDuration().
    if (sign == 0)
        return zero_date_duration(vm);

    // 3. If calendar is "iso8601", then
    if (calendar == ISO8601_CALENDAR) {
        // a. Set sign to -sign.
        sign *= -1;

        // b. Let years be 0.
        double years = 0;

        // d. Let months be 0.
        double months = 0;

        // OPTIMIZATION: If the largestUnit is MONTH, we want to skip ahead to the correct year. If implemented in exact
        //               accordance with the spec, we could enter the second ISODateSurpasses loop below with a very large
        //               number of months to traverse.

        // c. If largestUnit is YEAR, then
        // e. If largestUnit is either YEAR or MONTH, then
        if (largest_unit == Unit::Year || largest_unit == Unit::Month) {
            // c.i. Let candidateYears be sign.
            auto candidate_years = two.year - one.year;
            if (candidate_years != 0)
                candidate_years -= sign;

            // c.ii. Repeat, while ISODateSurpasses(sign, one, two, candidateYears, 0, 0, 0) is false,
            while (!iso_date_surpasses(vm, sign, one, two, candidate_years, 0, 0, 0)) {
                // 1. Set years to candidateYears.
                years = candidate_years;

                // 2. Set candidateYears to candidateYears + sign.
                candidate_years += sign;
            }

            // e.i. Let candidateMonths be sign.
            double candidate_months = sign;

            // e.ii. Repeat, while ISODateSurpasses(sign, one, two, years, candidateMonths, 0, 0) is false,
            while (!iso_date_surpasses(vm, sign, one, two, years, candidate_months, 0, 0)) {
                // 1. Set months to candidateMonths.
                months = candidate_months;

                // 2. Set candidateMonths to candidateMonths + sign.
                candidate_months += sign;
            }

            if (largest_unit == Unit::Month) {
                months += years * 12.0;
                years = 0.0;
            }
        }

        // f. Let weeks be 0.
        double weeks = 0;

        // OPTIMIZATION: If the largestUnit is DAY, we do not want to enter an ISODateSurpasses loop. The loop would have
        //               us increment the intermediate ISOYearMonth one day at time, which will take an extremely long
        //               time if the difference is a large number of years. Instead, we can compute the day difference,
        //               and convert to weeks if needed.
        auto year_month = balance_iso_year_month(static_cast<double>(one.year) + years, static_cast<double>(one.month) + months);
        auto regulated_date = MUST(regulate_iso_date(vm, year_month.year, year_month.month, one.day, Overflow::Constrain));

        auto days = iso_date_to_epoch_days(two.year, two.month - 1, two.day) - iso_date_to_epoch_days(regulated_date.year, regulated_date.month - 1, regulated_date.day);

        if (largest_unit == Unit::Week) {
            weeks = trunc(days / 7.0);
            days = fmod(days, 7.0);
        }

        // k. Return ! CreateDateDurationRecord(years, months, weeks, days).
        return MUST(create_date_duration_record(vm, years, months, weeks, days));
    }

    // 4. Return NonISODateUntil(calendar, one, two, largestUnit).
    return non_iso_date_until(vm, calendar, one, two, largest_unit);
}

// 12.3.10 ToTemporalCalendarIdentifier ( temporalCalendarLike ), https://tc39.es/proposal-temporal/#sec-temporal-totemporalcalendaridentifier
ThrowCompletionOr<String> to_temporal_calendar_identifier(VM& vm, Value temporal_calendar_like)
{
    // 1. If temporalCalendarLike is an Object and temporalCalendarLike has an [[InitializedTemporalDate]],
    //    [[InitializedTemporalDateTime]], [[InitializedTemporalMonthDay]], [[InitializedTemporalYearMonth]], or
    //    [[InitializedTemporalZonedDateTime]] internal slot, return temporalCalendarLike.[[Calendar]].
    if (auto plain_date = temporal_calendar_like.as_if<PlainDate>())
        return plain_date->calendar();
    if (auto plain_date_time = temporal_calendar_like.as_if<PlainDateTime>())
        return plain_date_time->calendar();
    if (auto plain_month_day = temporal_calendar_like.as_if<PlainMonthDay>())
        return plain_month_day->calendar();
    if (auto plain_year_month = temporal_calendar_like.as_if<PlainYearMonth>())
        return plain_year_month->calendar();
    if (auto zoned_date_time = temporal_calendar_like.as_if<ZonedDateTime>())
        return zoned_date_time->calendar();

    // 2. If temporalCalendarLike is not a String, throw a TypeError exception.
    if (!temporal_calendar_like.is_string())
        return vm.throw_completion<TypeError>(ErrorType::TemporalInvalidCalendar);

    // 3. Let identifier be ? ParseTemporalCalendarString(temporalCalendarLike).
    auto identifier = TRY(parse_temporal_calendar_string(vm, temporal_calendar_like.as_string().utf8_string()));

    // 4. Return ? CanonicalizeCalendar(identifier).
    return TRY(canonicalize_calendar(vm, identifier));
}

// 12.3.11 GetTemporalCalendarIdentifierWithISODefault ( item ), https://tc39.es/proposal-temporal/#sec-temporal-gettemporalcalendarslotvaluewithisodefault
ThrowCompletionOr<String> get_temporal_calendar_identifier_with_iso_default(VM& vm, Object const& item)
{
    // 1. If item has an [[InitializedTemporalDate]], [[InitializedTemporalDateTime]], [[InitializedTemporalMonthDay]],
    //    [[InitializedTemporalYearMonth]], or [[InitializedTemporalZonedDateTime]] internal slot, then
    //     a. Return item.[[Calendar]].
    if (auto const* plain_date = as_if<PlainDate>(item))
        return plain_date->calendar();
    if (auto const* plain_date_time = as_if<PlainDateTime>(item))
        return plain_date_time->calendar();
    if (auto const* plain_month_day = as_if<PlainMonthDay>(item))
        return plain_month_day->calendar();
    if (auto const* plain_year_month = as_if<PlainYearMonth>(item))
        return plain_year_month->calendar();
    if (auto const* zoned_date_time = as_if<ZonedDateTime>(item))
        return zoned_date_time->calendar();

    // 2. Let calendarLike be ? Get(item, "calendar").
    auto calendar_like = TRY(item.get(vm.names.calendar));

    // 3. If calendarLike is undefined, return "iso8601".
    if (calendar_like.is_undefined())
        return "iso8601"_string;

    // 4. Return ? ToTemporalCalendarIdentifier(calendarLike).
    return TRY(to_temporal_calendar_identifier(vm, calendar_like));
}

// 12.3.12 CalendarDateFromFields ( calendar, fields, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-calendardatefromfields
ThrowCompletionOr<ISODate> calendar_date_from_fields(VM& vm, String const& calendar, CalendarFields& fields, Overflow overflow)
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

// 12.3.13 CalendarYearMonthFromFields ( calendar, fields, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-calendaryearmonthfromfields
ThrowCompletionOr<ISODate> calendar_year_month_from_fields(VM& vm, String const& calendar, CalendarFields& fields, Overflow overflow)
{
    // 1. Set fields.[[Day]] to 1.
    fields.day = 1;

    // 2. Perform ? CalendarResolveFields(calendar, fields, YEAR-MONTH).
    TRY(calendar_resolve_fields(vm, calendar, fields, DateType::YearMonth));

    // 3. Let result be ? CalendarDateToISO(calendar, fields, overflow).
    auto result = TRY(calendar_date_to_iso(vm, calendar, fields, overflow));

    // 4. If ISOYearMonthWithinLimits(result) is false, throw a RangeError exception.
    if (!iso_year_month_within_limits(result))
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidISODate);

    // 5. Return result.
    return result;
}

// 12.3.14 CalendarMonthDayFromFields ( calendar, fields, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-calendarmonthdayfromfields
ThrowCompletionOr<ISODate> calendar_month_day_from_fields(VM& vm, String const& calendar, CalendarFields& fields, Overflow overflow)
{
    // 1. Perform ? CalendarResolveFields(calendar, fields, MONTH-DAY).
    TRY(calendar_resolve_fields(vm, calendar, fields, DateType::MonthDay));

    // 2. Let result be ? CalendarMonthDayToISOReferenceDate(calendar, fields, overflow).
    auto result = TRY(calendar_month_day_to_iso_reference_date(vm, calendar, fields, overflow));

    // 3. Assert: ISODateWithinLimits(result) is true.
    VERIFY(iso_date_within_limits(result));

    // 4. Return result.
    return result;
}

// 12.3.15 FormatCalendarAnnotation ( id, showCalendar ), https://tc39.es/proposal-temporal/#sec-temporal-formatcalendarannotation
String format_calendar_annotation(StringView id, ShowCalendar show_calendar)
{
    // 1. If showCalendar is NEVER, return the empty String.
    if (show_calendar == ShowCalendar::Never)
        return String {};

    // 2. If showCalendar is AUTO and id is "iso8601", return the empty String.
    if (show_calendar == ShowCalendar::Auto && id == ISO8601_CALENDAR)
        return String {};

    // 3. If showCalendar is CRITICAL, let flag be "!"; else, let flag be the empty String.
    auto flag = show_calendar == ShowCalendar::Critical ? "!"sv : ""sv;

    // 4. Return the string-concatenation of "[", flag, "u-ca=", id, and "]".
    return MUST(String::formatted("[{}u-ca={}]", flag, id));
}

// 12.3.16 CalendarEquals ( one, two ), https://tc39.es/proposal-temporal/#sec-temporal-calendarequals
bool calendar_equals(StringView one, StringView two)
{
    // 1. If CanonicalizeUValue("ca", one) is CanonicalizeUValue("ca", two), return true.
    // 2. Return false.
    return Unicode::canonicalize_unicode_extension_values("ca"sv, one)
        == Unicode::canonicalize_unicode_extension_values("ca"sv, two);
}

// 12.3.17 ISODaysInMonth ( year, month ), https://tc39.es/proposal-temporal/#sec-temporal-isodaysinmonth
u8 iso_days_in_month(double year, double month)
{
    // 1. If month is one of 1, 3, 5, 7, 8, 10, or 12, return 31.
    if (first_is_one_of(month, 1, 3, 5, 7, 8, 10, 12))
        return 31;

    // 2. If month is one of 4, 6, 9, or 11, return 30.
    if (first_is_one_of(month, 4, 6, 9, 11))
        return 30;

    // 3. Assert: month is 2.
    VERIFY(month == 2);

    // 4. Return 28 + MathematicalInLeapYear(EpochTimeForYear(year)).
    return 28 + mathematical_in_leap_year(epoch_time_for_year(year));
}

// 12.3.18 ISOWeekOfYear ( isoDate ), https://tc39.es/proposal-temporal/#sec-temporal-isoweekofyear
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

// 12.3.19 ISODayOfYear ( isoDate ), https://tc39.es/proposal-temporal/#sec-temporal-isodayofyear
u16 iso_day_of_year(ISODate iso_date)
{
    // 1. Let epochDays be ISODateToEpochDays(isoDate.[[Year]], isoDate.[[Month]] - 1, isoDate.[[Day]]).
    auto epoch_days = iso_date_to_epoch_days(iso_date.year, iso_date.month - 1, iso_date.day);

    // 2. Return EpochTimeToDayInYear(EpochDaysToEpochMs(epochDays, 0)) + 1.
    return epoch_time_to_day_in_year(epoch_days_to_epoch_ms(epoch_days, 0)) + 1;
}

// 12.3.20 ISODayOfWeek ( isoDate ), https://tc39.es/proposal-temporal/#sec-temporal-isodayofweek
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

// 12.3.21 NonISOCalendarDateToISO ( calendar, fields, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-nonisocalendardatetoiso
// 4.1.20 NonISOCalendarDateToISO ( calendar, fields, overflow ), https://tc39.es/proposal-intl-era-monthcode/#sup-temporal-nonisocalendardatetoiso
ThrowCompletionOr<ISODate> non_iso_calendar_date_to_iso(VM& vm, String const& calendar, CalendarFields const& fields, Overflow overflow)
{
    // 1. Assert: fields.[[Year]], fields.[[Month]], and fields.[[Day]] are not UNSET.
    VERIFY(fields.year.has_value());
    VERIFY(fields.month.has_value());
    VERIFY(fields.day.has_value());

    // 2. If fields.[[MonthCode]] is not UNSET, then
    if (fields.month_code.has_value()) {
        // a. Perform ? ConstrainMonthCode(calendar, fields.[[Year]], fields.[[MonthCode]], overflow).
        (void)TRY(constrain_month_code(vm, calendar, *fields.year, *fields.month_code, overflow));
    }

    // 3. Let monthsInYear be CalendarMonthsInYear(calendar, fields.[[Year]]).
    auto months_in_year = calendar_months_in_year(calendar, *fields.year);

    u8 month = 0;
    u8 day = 0;

    // 4. If fields.[[Month]] > monthsInYear, then
    if (*fields.month > months_in_year) {
        // a. If overflow is REJECT, throw a RangeError exception.
        if (overflow == Overflow::Reject)
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFieldName, "month"sv);

        // b. Let month be monthsInYear.
        month = months_in_year;
    }
    // 5. Else,
    else {
        // a. Let month be fields.[[Month]].
        month = static_cast<u8>(*fields.month);
    }

    // 6. Let daysInMonth be CalendarDaysInMonth(calendar, fields.[[Year]], fields.[[Month]]).
    // FIXME: Spec issue: We should use the `month` value that we just constrained.
    auto days_in_month = calendar_days_in_month(calendar, *fields.year, month);

    // 7. If fields.[[Day]] > daysInMonth, then
    if (*fields.day > days_in_month) {
        // a. If overflow is REJECT, throw a RangeError exception.
        if (overflow == Overflow::Reject)
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFieldName, "day"sv);

        // b. Let day be daysInMonth.
        day = days_in_month;
    }
    // 8. Else,
    else {
        // a. Let day be fields.[[Day]].
        day = static_cast<u8>(*fields.day);
    }

    // 9. Return ? CalendarIntegersToISO(calendar, fields.[[Year]], month, day).
    return TRY(calendar_integers_to_iso(vm, calendar, *fields.year, month, day));
}

// 12.3.22 CalendarDateToISO ( calendar, fields, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-calendardatetoiso
ThrowCompletionOr<ISODate> calendar_date_to_iso(VM& vm, String const& calendar, CalendarFields const& fields, Overflow overflow)
{
    // 1. If calendar is "iso8601", then
    if (calendar == ISO8601_CALENDAR) {
        // a. Assert: fields.[[Year]], fields.[[Month]], and fields.[[Day]] are not UNSET.
        VERIFY(fields.year.has_value());
        VERIFY(fields.month.has_value());
        VERIFY(fields.day.has_value());

        // b. Return ? RegulateISODate(fields.[[Year]], fields.[[Month]], fields.[[Day]], overflow).
        return TRY(regulate_iso_date(vm, *fields.year, *fields.month, *fields.day, overflow));
    }

    // 2. Return ? NonISOCalendarDateToISO(calendar, fields, overflow).
    return non_iso_calendar_date_to_iso(vm, calendar, fields, overflow);
}

// 12.3.23 NonISOMonthDayToISOReferenceDate ( calendar, fields, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-nonisomonthdaytoisoreferencedate
// 4.1.21 NonISOMonthDayToISOReferenceDate ( calendar, fields, overflow ), https://tc39.es/proposal-intl-era-monthcode/#sup-temporal-nonisomonthdaytoisoreferencedate
ThrowCompletionOr<ISODate> non_iso_month_day_to_iso_reference_date(VM& vm, String const& calendar, CalendarFields const& fields, Overflow overflow)
{
    // 1. Assert: fields.[[Day]] is not UNSET.
    VERIFY(fields.day.has_value());

    u8 day = 0;
    u8 month = 0;
    u8 days_in_month = 0;
    String month_code;

    // 2. If fields.[[Year]] is not UNSET, then
    if (fields.year.has_value()) {
        // a. Assert: fields.[[Month]] is not UNSET.
        VERIFY(fields.month.has_value());

        // b. If there exists no combination of inputs such that ! CalendarIntegersToISO(calendar, fields.[[Year]], ..., ...)
        //    would return an ISO Date Record isoDate for which ISODateWithinLimits(isoDate) is true, throw a RangeError exception.
        // c. NOTE: The above step exists so as not to require calculating whether the month and day described in fields
        //    exist in user-provided years arbitrarily far in the future or past.
        if (auto iso_date = MUST(calendar_integers_to_iso(vm, calendar, *fields.year, 1, 1)); !iso_date_within_limits(iso_date))
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidISODate);

        // d. Let monthsInYear be CalendarMonthsInYear(calendar, fields.[[Year]]).
        auto months_in_year = calendar_months_in_year(calendar, *fields.year);

        // e. If fields.[[Month]] > monthsInYear, then
        if (*fields.month > months_in_year) {
            // i. If overflow is REJECT, throw a RangeError exception.
            if (overflow == Overflow::Reject)
                return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFieldName, "month"sv);

            // ii. Let month be monthsInYear.
            month = months_in_year;
        }
        // f. Else,
        else {
            // i. Let month be fields.[[Month]].
            month = static_cast<u8>(*fields.month);
        }

        // g. If fields.[[MonthCode]] is UNSET, then
        if (!fields.month_code.has_value()) {
            // i. Let fieldsISODate be ! CalendarIntegersToISO(calendar, fields.[[Year]], month, 1).
            auto fields_iso_date = MUST(calendar_integers_to_iso(vm, calendar, *fields.year, month, 1));

            // ii. Let monthCode be NonISOCalendarISOToDate(calendar, fieldsISODate).[[MonthCode]].
            month_code = non_iso_calendar_iso_to_date(calendar, fields_iso_date).month_code;
        }
        // h. Else,
        else {
            // i. Let monthCode be ? ConstrainMonthCode(calendar, fields.[[Year]], fields.[[MonthCode]], overflow).
            month_code = TRY(constrain_month_code(vm, calendar, *fields.year, *fields.month_code, overflow));
        }

        // i. Let daysInMonth be CalendarDaysInMonth(calendar, fields.[[Year]], month).
        days_in_month = calendar_days_in_month(calendar, *fields.year, month);
    }
    // 3. Else,
    else {
        // a. Assert: fields.[[MonthCode]] is not UNSET.
        VERIFY(fields.month_code.has_value());

        // b. Let monthCode be fields.[[MonthCode]].
        month_code = *fields.month_code;

        // c. If calendar is "chinese" or "dangi", let daysInMonth be 30; else, let daysInMonth be the maximum number of
        //    days in the month described by monthCode in any year.
        days_in_month = calendar.is_one_of("chinese"sv, "dangi"sv) ? 30 : Unicode::calendar_max_days_in_month_code(calendar, month_code);
    }

    // 4. If fields.[[Day]] > daysInMonth, then
    if (*fields.day > days_in_month) {
        // a. If overflow is REJECT, throw a RangeError exception.
        if (overflow == Overflow::Reject)
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFieldName, "day"sv);

        // b. Let day be daysInMonth.
        day = days_in_month;
    }
    // 5. Else,
    else {
        // a. Let day be fields.[[Day]].
        day = static_cast<u8>(*fields.day);
    }

    auto is_chinese_or_dangi = calendar.is_one_of("chinese"sv, "dangi"sv);

    // 6. If calendar is "chinese" or "dangi", then
    if (is_chinese_or_dangi) {
        // a. NOTE: This special case handles combinations of month and day that theoretically could occur but are not
        //    known to have occurred historically and cannot be accurately calculated to occur in the future, even if it
        //    may be possible to construct a PlainDate with such combinations due to inaccurate approximations. This is
        //    explicitly mentioned here because as time goes on, these dates may become known to have occurred
        //    historically, or may be more accurately calculated to occur in the future.

        // b. Let row be the row in Table 6 with a value in the "Month Code" column matching monthCode.
        auto reference_year = chinese_or_dangi_reference_year(calendar, month_code, day);

        // c. If the "Reference Year (Days 1-29)" column of row is "—", or day = 30 and the "Reference Year (Day 30)"
        //    column of row is "—", then
        if (!reference_year.has_value()) {
            // i. If overflow is REJECT, throw a RangeError exception.
            if (overflow == Overflow::Reject)
                return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFieldName, "monthCode"sv);

            // ii. Set monthCode to CreateMonthCode(! ParseMonthCode(monthCode).[[MonthNumber]], false).
            if (month_code.ends_with('L'))
                month_code = MUST(month_code.trim("L"sv, TrimMode::Right));
        }
    }

    // 7. Let referenceYear be the ISO reference year for monthCode and day as described above. If calendar is "chinese"
    //    or "dangi", the reference years in Table 6 are to be used.
    // 8. Return the latest possible ISO Date Record isoDate such that isoDate.[[Year]] = referenceYear and
    //    NonISOCalendarISOToDate(calendar, isoDate) returns a Calendar Date Record whose [[MonthCode]] and [[Day]]
    //    field values respectively equal monthCode and day.
    auto result = [&]() -> Optional<ISODate> {
        if (is_chinese_or_dangi) {
            auto reference_year = chinese_or_dangi_reference_year(calendar, month_code, day);
            return Unicode::calendar_month_code_to_iso_date(calendar, *reference_year, month_code, day);
        }

        for (i32 iso_year = 1972; iso_year >= 1900; --iso_year) {
            if (auto result = Unicode::calendar_month_code_to_iso_date(calendar, iso_year, month_code, day); result.has_value())
                return result;
        }
        for (i32 iso_year = 1973; iso_year <= 2035; ++iso_year) {
            if (auto result = Unicode::calendar_month_code_to_iso_date(calendar, iso_year, month_code, day); result.has_value())
                return result;
        }

        return {};
    }();

    VERIFY(result.has_value());
    return *result;
}

// 12.3.24 CalendarMonthDayToISOReferenceDate ( calendar, fields, overflow ), https://tc39.es/proposal-temporal/#sec-temporal-calendarmonthdaytoisoreferencedate
ThrowCompletionOr<ISODate> calendar_month_day_to_iso_reference_date(VM& vm, String const& calendar, CalendarFields const& fields, Overflow overflow)
{
    // 1. If calendar is "iso8601", then
    if (calendar == ISO8601_CALENDAR) {
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

    // 2. Return ? NonISOMonthDayToISOReferenceDate(calendar, fields, overflow).
    return non_iso_month_day_to_iso_reference_date(vm, calendar, fields, overflow);
}

// 12.3.25 NonISOCalendarISOToDate ( calendar, isoDate ), https://tc39.es/proposal-temporal/#sec-temporal-nonisocalendarisotodate
CalendarDate non_iso_calendar_iso_to_date(String const& calendar, ISODate iso_date)
{
    auto result = Unicode::iso_date_to_calendar_date(calendar, iso_date);

    for (auto const& row : CALENDAR_ERA_DATA) {
        if (row.calendar != calendar)
            continue;

        i32 era_year = 0;

        switch (row.kind) {
        case CalendarEraData::Kind::Epoch:
            era_year = result.year;
            break;
        case CalendarEraData::Kind::Negative:
            era_year = 1 - result.year;
            break;
        case CalendarEraData::Kind::Offset:
            era_year = result.year - *row.offset + 1;
            break;
        }

        if (row.minimum_era_year.has_value() && era_year < *row.minimum_era_year)
            continue;
        if (row.maximum_era_year.has_value() && era_year > *row.maximum_era_year)
            continue;
        if (row.iso_era_start.has_value() && compare_iso_date(iso_date, *row.iso_era_start) < 0)
            continue;

        result.era = String::from_utf8_without_validation(row.era.bytes());
        result.era_year = era_year;
        break;
    }

    return result;
}

// 12.3.26 CalendarISOToDate ( calendar, isoDate ), https://tc39.es/proposal-temporal/#sec-temporal-calendarisotodate
CalendarDate calendar_iso_to_date(String const& calendar, ISODate iso_date)
{
    // 1. If calendar is "iso8601", then
    if (calendar == ISO8601_CALENDAR) {
        // a. If MathematicalInLeapYear(EpochTimeForYear(isoDate.[[Year]])) = 1, let inLeapYear be true; else let inLeapYear be false.
        auto in_leap_year = mathematical_in_leap_year(epoch_time_for_year(iso_date.year)) == 1;

        // b. Return Calendar Date Record { [[Era]]: undefined, [[EraYear]]: undefined, [[Year]]: isoDate.[[Year]],
        //    [[Month]]: isoDate.[[Month]], [[MonthCode]]: CreateMonthCode(isoDate.[[Month]], false), [[Day]]: isoDate.[[Day]],
        //    [[DayOfWeek]]: ISODayOfWeek(isoDate), [[DayOfYear]]: ISODayOfYear(isoDate), [[WeekOfYear]]: ISOWeekOfYear(isoDate),
        //    [[DaysInWeek]]: 7, [[DaysInMonth]]: ISODaysInMonth(isoDate.[[Year]], isoDate.[[Month]]),
        //    [[DaysInYear]]: MathematicalDaysInYear(isoDate.[[Year]]), [[MonthsInYear]]: 12, [[InLeapYear]]: inLeapYear }.
        return CalendarDate {
            .era = {},
            .era_year = {},
            .year = iso_date.year,
            .month = iso_date.month,
            .month_code = create_month_code(iso_date.month, false),
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

    // 2. Return NonISOCalendarISOToDate(calendar, isoDate).
    return non_iso_calendar_iso_to_date(calendar, iso_date);
}

// 12.3.27 CalendarExtraFields ( calendar, fields ), https://tc39.es/proposal-temporal/#sec-temporal-calendarextrafields
// 4.1.22 CalendarExtraFields ( calendar, fields ), https://tc39.es/proposal-intl-era-monthcode/#sup-temporal-calendarextrafields
Vector<CalendarField> calendar_extra_fields(String const& calendar, CalendarFieldList fields)
{
    // 1. If fields contains an element equal to YEAR and CalendarSupportsEra(calendar) is true, then
    if (fields.contains_slow(CalendarField::Year) && calendar_supports_era(calendar)) {
        // a. Return « ERA, ERA-YEAR ».
        return { CalendarField::Era, CalendarField::EraYear };
    }

    // 2. Return an empty List.
    return {};
}

// 12.3.28 NonISOFieldKeysToIgnore ( calendar, keys ), https://tc39.es/proposal-temporal/#sec-temporal-nonisofieldkeystoignore
// 4.1.23 NonISOFieldKeysToIgnore ( calendar, keys ), https://tc39.es/proposal-intl-era-monthcode/#sup-temporal-nonisofieldkeystoignore
Vector<CalendarField> non_iso_field_keys_to_ignore(String const& calendar, ReadonlySpan<CalendarField> keys)
{
    // 1. Let ignoredKeys be a copy of keys.
    Vector<CalendarField> ignored_keys { keys };

    // 2. For each element key of keys, do
    for (auto key : keys) {
        // a. If key is MONTH, append MONTH-CODE to ignoredKeys.
        if (key == CalendarField::Month)
            ignored_keys.append(CalendarField::MonthCode);

        // b. If key is MONTH-CODE, append month.
        if (key == CalendarField::MonthCode)
            ignored_keys.append(CalendarField::Month);

        // c. If key is one of ERA, ERA-YEAR, or YEAR and CalendarSupportsEra(calendar) is true, then
        if (first_is_one_of(key, CalendarField::Era, CalendarField::EraYear, CalendarField::Year) && calendar_supports_era(calendar)) {
            // i. Append ERA, ERA-YEAR, and YEAR to ignoredKeys.
            ignored_keys.append(CalendarField::Era);
            ignored_keys.append(CalendarField::EraYear);
            ignored_keys.append(CalendarField::Year);
        }

        // d. If key is one of DAY, MONTH, or MONTH-CODE and CalendarHasMidYearEras(calendar) is true, then
        if (first_is_one_of(key, CalendarField::Day, CalendarField::Month, CalendarField::MonthCode) && calendar_has_mid_year_eras(calendar)) {
            // i. Append ERA and ERA-YEAR to ignoredKeys.
            ignored_keys.append(CalendarField::Era);
            ignored_keys.append(CalendarField::EraYear);
        }
    }

    // 3. NOTE: While ignoredKeys can have duplicate elements, this is not intended to be meaningful. This specification
    //    only checks whether particular keys are or are not members of the list.

    // 4. Return ignoredKeys.
    return ignored_keys;
}

// 12.3.29 CalendarFieldKeysToIgnore ( calendar, keys ), https://tc39.es/proposal-temporal/#sec-temporal-calendarfieldkeystoignore
Vector<CalendarField> calendar_field_keys_to_ignore(String const& calendar, ReadonlySpan<CalendarField> keys)
{
    // 1. If calendar is "iso8601", then
    if (calendar == ISO8601_CALENDAR) {
        // a. Let ignoredKeys be a new empty List.
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

    // 2. Return NonISOFieldKeysToIgnore(calendar, keys).
    return non_iso_field_keys_to_ignore(calendar, keys);
}

// 12.3.30 NonISOResolveFields ( calendar, fields, type ), https://tc39.es/proposal-temporal/#sec-temporal-nonisoresolvefields
// 4.1.24 NonISOResolveFields ( calendar, fields, type ), https://tc39.es/proposal-intl-era-monthcode/#sup-temporal-nonisoresolvefields
ThrowCompletionOr<void> non_iso_resolve_fields(VM& vm, String const& calendar, CalendarFields& fields, DateType type)
{
    // 1. Let needsYear be false.
    // 2. If type is DATE or type is YEAR-MONTH, set needsYear to true.
    // 3. If fields.[[MonthCode]] is UNSET, set needsYear to true.
    // 4. If fields.[[Month]] is not UNSET, set needsYear to true.
    auto needs_year = type == DateType::Date || type == DateType::YearMonth
        || !fields.month_code.has_value()
        || fields.month.has_value();

    // 5. Let needsOrdinalMonth be false.
    // 6. If fields.[[Year]] is not UNSET, set needsOrdinalMonth to true.
    // 7. If fields.[[EraYear]] is not UNSET, set needsOrdinalMonth to true.
    auto needs_ordinal_month = fields.year.has_value() || fields.era_year.has_value();

    // 8. Let needsDay be false.
    // 9. If type is DATE or type is MONTH-DAY, set needsDay to true.
    auto needs_day = type == DateType::Date || type == DateType::MonthDay;

    // 10. If needsYear is true, then
    if (needs_year) {
        // a. If fields.[[Year]] is UNSET, then
        if (!fields.year.has_value()) {
            // i. If CalendarSupportsEra(calendar) is false, throw a TypeError exception.
            if (!calendar_supports_era(calendar))
                return vm.throw_completion<TypeError>(ErrorType::MissingRequiredProperty, "year"sv);

            // ii. If fields.[[Era]] is UNSET or fields.[[EraYear]] is UNSET, throw a TypeError exception.
            if (!fields.era.has_value() || !fields.era_year.has_value())
                return vm.throw_completion<TypeError>(ErrorType::MissingRequiredProperty, "era"sv);
        }
    }

    // 11. If CalendarSupportsEra(calendar) is true, then
    if (calendar_supports_era(calendar)) {
        // a. If fields.[[Era]] is not UNSET and fields.[[EraYear]] is UNSET, throw a TypeError exception.
        if (fields.era.has_value() && !fields.era_year.has_value())
            return vm.throw_completion<TypeError>(ErrorType::MissingRequiredProperty, "eraYear"sv);

        // b. If fields.[[EraYear]] is not UNSET and fields.[[Era]] is UNSET, throw a TypeError exception.
        if (fields.era_year.has_value() && !fields.era.has_value())
            return vm.throw_completion<TypeError>(ErrorType::MissingRequiredProperty, "era"sv);
    }

    // 12. If needsDay is true and fields.[[Day]] is UNSET, throw a TypeError exception.
    if (needs_day && !fields.day.has_value())
        return vm.throw_completion<TypeError>(ErrorType::MissingRequiredProperty, "day"sv);

    // 13. If fields.[[Month]] is UNSET and fields.[[MonthCode]] is UNSET, throw a TypeError exception.
    if (!fields.month.has_value() && !fields.month_code.has_value())
        return vm.throw_completion<TypeError>(ErrorType::MissingRequiredProperty, "month"sv);

    // 14. If CalendarSupportsEra(calendar) is true and fields.[[EraYear]] is not UNSET, then
    if (calendar_supports_era(calendar) && fields.era_year.has_value()) {
        // a. Let canonicalEra be CanonicalizeEraInCalendar(calendar, fields.[[Era]]).
        auto canonical_era = canonicalize_era_in_calendar(calendar, *fields.era);

        // b. If canonicalEra is undefined, throw a RangeError exception.
        if (!canonical_era.has_value())
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFieldName, "era"sv);

        // c. Let arithmeticYear be CalendarDateArithmeticYearForEraYear(calendar, canonicalEra, fields.[[EraYear]]).
        auto arithmetic_year = calendar_date_arithmetic_year_for_era_year(calendar, *canonical_era, *fields.era_year);

        // d. If fields.[[Year]] is not UNSET, and fields.[[Year]] ≠ arithmeticYear, throw a RangeError exception.
        if (fields.year.has_value() && *fields.year != arithmetic_year)
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFieldName, "year"sv);

        // e. Set fields.[[Year]] to arithmeticYear.
        fields.year = arithmetic_year;
    }

    // 15. Set fields.[[Era]] to UNSET.
    fields.era = {};

    // 16. Set fields.[[EraYear]] to UNSET.
    fields.era_year = {};

    // 17. NOTE: fields.[[Era]] and fields.[[EraYear]] are erased in order to allow a lenient interpretation of
    //     out-of-bounds values, which is particularly useful for consistent interpretation of dates in calendars with
    //     regnal eras.

    // 18. If fields.[[MonthCode]] is not UNSET, then
    if (fields.month_code.has_value()) {
        // a. If IsValidMonthCodeForCalendar(calendar, fields.[[MonthCode]]) is false, throw a RangeError exception.
        if (!is_valid_month_code_for_calendar(calendar, *fields.month_code))
            return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFieldName, "monthCode"sv);

        // b. If fields.[[Year]] is not UNSET, then
        if (fields.year.has_value()) {
            // i. If YearContainsMonthCode(calendar, fields.[[Year]], fields.[[MonthCode]]) is true, let constrainedMonthCode be fields.[[MonthCode]];
            //    else let constrainedMonthCode be ! ConstrainMonthCode(calendar, fields.[[Year]], fields.[[MonthCode]], CONSTRAIN).
            auto constrained_month_code = year_contains_month_code(calendar, *fields.year, *fields.month_code)
                ? *fields.month_code
                : MUST(constrain_month_code(vm, calendar, *fields.year, *fields.month_code, Overflow::Constrain));

            // ii. Let month be MonthCodeToOrdinal(calendar, fields.[[Year]], constrainedMonthCode).
            auto month = month_code_to_ordinal(calendar, *fields.year, constrained_month_code);

            // iii. If fields.[[Month]] is not UNSET and fields.[[Month]] ≠ month, throw a RangeError exception.
            if (fields.month.has_value() && *fields.month != month)
                return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFieldName, "month"sv);

            // iv. Set fields.[[Month]] to month.
            fields.month = month;

            // v. NOTE: fields.[[MonthCode]] is intentionally not overwritten with constrainedMonthCode. Pending the
            //    "overflow" parameter in CalendarDateToISO or CalendarMonthDayToISOReferenceDate, a month code not
            //    occurring in fields.[[Year]] may cause that operation to throw. However, if fields.[[Month]] is
            //    present, it must agree with the constrained month code.
        }
    }

    // 19. Assert: fields.[[Era]] and fields.[[EraYear]] are UNSET.
    VERIFY(!fields.era.has_value());
    VERIFY(!fields.era_year.has_value());

    // 20. Assert: If needsYear is true, fields.[[Year]] is not UNSET.
    if (needs_year)
        VERIFY(fields.year.has_value());

    // 21. Assert: If needsOrdinalMonth is true, fields.[[Month]] is not UNSET.
    if (needs_ordinal_month)
        VERIFY(fields.month.has_value());

    // 22. Assert: If needsDay is true, fields.[[Day]] is not UNSET.
    if (needs_day)
        VERIFY(fields.day.has_value());

    // 23. Return unused.
    return {};
}

// 12.3.31 CalendarResolveFields ( calendar, fields, type ), https://tc39.es/proposal-temporal/#sec-temporal-calendarresolvefields
ThrowCompletionOr<void> calendar_resolve_fields(VM& vm, String const& calendar, CalendarFields& fields, DateType type)
{
    // 1. If calendar is "iso8601", then
    if (calendar == ISO8601_CALENDAR) {
        // a. Let needsYear be false.
        // b. If type is either DATE or YEAR-MONTH, set needsYear to true.
        auto needs_year = type == DateType::Date || type == DateType::YearMonth;

        // c. Let needsDay be false.
        // d. If type is either DATE or MONTH-DAY, set needsDay to true.
        auto needs_day = type == DateType::Date || type == DateType::MonthDay;

        // e. If needsYear is true and fields.[[Year]] is UNSET, throw a TypeError exception.
        if (needs_year && !fields.year.has_value())
            return vm.throw_completion<TypeError>(ErrorType::MissingRequiredProperty, "year"sv);

        // f. If needsDay is true and fields.[[Day]] is UNSET, throw a TypeError exception.
        if (needs_day && !fields.day.has_value())
            return vm.throw_completion<TypeError>(ErrorType::MissingRequiredProperty, "day"sv);

        // g. If fields.[[Month]] is UNSET and fields.[[MonthCode]] is UNSET, throw a TypeError exception.
        if (!fields.month.has_value() && !fields.month_code.has_value())
            return vm.throw_completion<TypeError>(ErrorType::MissingRequiredProperty, "month"sv);

        // h. If fields.[[MonthCode]] is not UNSET, then
        if (fields.month_code.has_value()) {
            // i. Let parsedMonthCode be ! ParseMonthCode(fields.[[MonthCode]]).
            auto parsed_month_code = parse_month_code(*fields.month_code);

            // ii. If parsedMonthCode.[[IsLeapMonth]] is true, throw a RangeError exception.
            if (parsed_month_code.is_leap_month)
                return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFieldName, "monthCode"sv);

            // iii. Let month be parsedMonthCode.[[MonthNumber]].
            auto month = parsed_month_code.month_number;

            // iv. If month > 12, throw a RangeError exception.
            if (month > 12)
                return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFieldName, "monthCode"sv);

            // v. If fields.[[Month]] is not UNSET and fields.[[Month]] ≠ month, throw a RangeError exception.
            if (fields.month.has_value() && fields.month != month)
                return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidCalendarFieldName, "month"sv);

            // vi. Set fields.[[Month]] to month.
            fields.month = month;
        }
    }
    // 2. Else,
    else {
        // a. Perform ? NonISOResolveFields(calendar, fields, type).
        return TRY(non_iso_resolve_fields(vm, calendar, fields, type));
    }

    // 3. Return UNUSED.
    return {};
}

// 4.1.1 CalendarSupportsEra ( calendar ), https://tc39.es/proposal-intl-era-monthcode/#sec-temporal-calendarsupportsera
bool calendar_supports_era(String const& calendar)
{
    // 1. If calendar is listed in the "Calendar" column of Table 2, return true.
    // 2. If calendar is listed in the "Calendar Type" column of Table 1, return false.
    // 3. Return an implementation-defined value.
    return find_value(CALENDAR_ERA_DATA, [&](auto const& row) { return row.calendar == calendar; }).has_value();
}

// 4.1.2 CanonicalizeEraInCalendar ( calendar, era ), https://tc39.es/proposal-intl-era-monthcode/#sec-temporal-canonicalizeeraincalendar
Optional<StringView> canonicalize_era_in_calendar(String const& calendar, StringView era)
{
    // 1. For each row of Table 2, except the header row, do
    for (auto const& row : CALENDAR_ERA_DATA) {
        // a. Let cal be the Calendar value of the current row.
        // b. If cal is equal to calendar, then
        if (row.calendar == calendar) {
            // i. Let canonicalName be the Era value of the current row.
            auto canonical_name = row.era;

            // ii. If canonicalName is equal to era, return canonicalName.
            if (canonical_name == era)
                return canonical_name;

            // iii. Let aliases be a List whose elements are the strings given in the "Aliases" column of the row.
            // iv. If aliases contains era, return canonicalName.
            if (row.alias == era)
                return canonical_name;
        }
    }

    // 2. If calendar is listed in the "Calendar Type" column of Table 1, return undefined.
    // 3. Return an implementation-defined value.
    return {};
}

// 4.1.3 CalendarHasMidYearEras ( calendar ), https://tc39.es/proposal-intl-era-monthcode/#sec-temporal-calendarhasmidyeareras
bool calendar_has_mid_year_eras(String const& calendar)
{
    // 1. If calendar is "japanese", return true.
    // 2. If calendar is listed in the "Calendar Type" column of Table 1, return false.
    // 3. Return an implementation-defined value.
    return calendar == "japanese"sv;
}

// 4.1.4 IsValidMonthCodeForCalendar ( calendar, monthCode ), https://tc39.es/proposal-intl-era-monthcode/#sec-temporal-isvalidmonthcodeforcalendar
bool is_valid_month_code_for_calendar(String const& calendar, StringView month_code)
{
    // 1. Let commonMonthCodes be « "M01", "M02", "M03", "M04", "M05", "M06", "M07", "M08", "M09", "M10", "M11", "M12" ».
    // 2. If commonMonthCodes contains monthCode, return true.
    if (month_code.is_one_of("M01"sv, "M02"sv, "M03"sv, "M04"sv, "M05"sv, "M06"sv, "M07"sv, "M08"sv, "M09"sv, "M10"sv, "M11"sv, "M12"sv))
        return true;

    // 3. If calendar is listed in the "Calendar" column of Table 3, then
    if (auto row = find_value(ADDITIONAL_MONTH_CODES, [&](auto const& row) { return row.calendar == calendar; }); row.has_value()) {
        // a. Let r be the row in Table 3 with a value in the Calendar column matching calendar.
        // b. Let specialMonthCodes be a List whose elements are the strings given in the "Additional Month Codes" column of r.
        // c. If specialMonthCodes contains monthCode, return true.
        // d. Return false.
        return row->additional_month_codes.contains_slow(month_code);
    }

    // 4. If calendar is listed in the "Calendar Type" column of Table 1, return false.
    // 5. Return an implementation-defined value.
    return false;
}

// 4.1.5 YearContainsMonthCode ( calendar, arithmeticYear, monthCode ), https://tc39.es/proposal-intl-era-monthcode/#sec-temporal-yearcontainsmonthcode
bool year_contains_month_code(String const& calendar, i32 arithmetic_year, StringView month_code)
{
    // 1. Assert: IsValidMonthCodeForCalendar(calendar, monthCode) is true.
    VERIFY(is_valid_month_code_for_calendar(calendar, month_code));

    // 2. If ! ParseMonthCode(monthCode).[[IsLeap]] is false, return true.
    auto [month_number, is_leap_month] = parse_month_code(month_code);
    if (!is_leap_month)
        return true;

    // 3. Return whether the leap month indicated by monthCode exists in the year arithmeticYear in calendar, using
    //    calendar-dependent behaviour.
    if (calendar.is_one_of("chinese"sv, "dangi"sv)) {
        auto months_in_year = calendar_months_in_year(calendar, arithmetic_year);
        if (months_in_year <= 12)
            return false;

        // Check each ordinal month to see if it matches the leap month code.
        for (u8 month = 1; month <= months_in_year; ++month) {
            auto info = Unicode::chinese_ordinal_month_code(calendar, arithmetic_year, month);
            if (info.has_value() && info->is_leap_month && info->month_number == month_number)
                return true;
        }

        return false;
    }

    if (calendar == "hebrew"sv) {
        if (month_number != Unicode::HEBREW_ADAR_I_MONTH_NUMBER)
            return false;

        auto months_in_year = calendar_months_in_year(calendar, arithmetic_year);
        return months_in_year == 13;
    }

    return false;
}

// 4.1.6 ConstrainMonthCode ( calendar, arithmeticYear, monthCode, overflow ), https://tc39.es/proposal-intl-era-monthcode/#sec-temporal-constrainmonthcode
ThrowCompletionOr<String> constrain_month_code(VM& vm, String const& calendar, i32 arithmetic_year, String const& month_code, Overflow overflow)
{
    // 1. Assert: IsValidMonthCodeForCalendar(calendar, monthCode) is true.
    VERIFY(is_valid_month_code_for_calendar(calendar, month_code));

    // 2. If YearContainsMonthCode(calendar, arithmeticYear, monthCode) is true, return monthCode.
    if (year_contains_month_code(calendar, arithmetic_year, month_code))
        return month_code;

    // 3. If overflow is REJECT, throw a RangeError exception.
    if (overflow == Overflow::Reject)
        return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidMonthCode);

    // 4. Assert: calendar is listed in the "Calendar" column of Table 3.
    // 5. Let r be the row in Table 3 with a value in the Calendar column matching calendar.
    auto row = find_value(ADDITIONAL_MONTH_CODES, [&](auto const& row) { return row.calendar == calendar; });
    VERIFY(row.has_value());

    // 6. Let shiftType be the value given in the "Leap to Common Month Transformation" column of r.
    // 7. If shiftType is SKIP-BACKWARD, then
    if (row->leap_to_common_month_transformation == AdditionalMonthCodes::Leap::SkipBackward) {
        // a. Return CreateMonthCode(! ParseMonthCode(monthCode).[[MonthNumber]], false).
        return MUST(month_code.trim("L"sv, TrimMode::Right));
    }

    // 8. Else,
    // a. Assert: monthCode is "M05L".
    VERIFY(month_code == "M05L");

    // b. Return "M06".
    return "M06"_string;
}

// 4.1.7 MonthCodeToOrdinal ( calendar, arithmeticYear, monthCode ), https://tc39.es/proposal-intl-era-monthcode/#sec-temporal-monthcodetoordinal
u8 month_code_to_ordinal(String const& calendar, i32 arithmetic_year, StringView month_code)
{
    // 1. Assert: YearContainsMonthCode(calendar, arithmeticYear, monthCode) is true.
    VERIFY(year_contains_month_code(calendar, arithmetic_year, month_code));

    // 2. Let monthsBefore be 0.
    auto months_before = 0;

    // 3. Let number be 1.
    auto number = 1;

    // 4. Let isLeap be false.
    auto is_leap = false;

    // 5. Let r be the row in Table 3 which the calendar is in the Calendar column.
    auto row = find_value(ADDITIONAL_MONTH_CODES, [&](auto const& row) { return row.calendar == calendar; });

    // 6. If the "Leap to Common Month Transformation" column of r is empty, then
    if (!row.has_value() || !row->leap_to_common_month_transformation.has_value()) {
        // a. Return ! ParseMonthCode(monthCode).[[MonthNumber]].
        return parse_month_code(month_code).month_number;
    }

    // 7. Assert: The "Additional Month Codes" column of r does not contain "M00L" or "M13".
    VERIFY(!row->additional_month_codes.contains_slow("M00L"sv));
    VERIFY(!row->additional_month_codes.contains_slow("M13"sv));

    // 8. Assert: This algorithm will return before the following loop terminates by failing its condition.

    // 9. Repeat, while number ≤ 12,
    while (number <= 12) {
        // a. Let currentMonthCode be CreateMonthCode(number, isLeap).
        auto current_month_code = create_month_code(number, is_leap);

        // b. If IsValidMonthCodeForCalendar(calendar, currentMonthCode) is true and YearContainsMonthCode(calendar, arithmeticYear, currentMonthCode) is true, then
        if (is_valid_month_code_for_calendar(calendar, current_month_code) && year_contains_month_code(calendar, arithmetic_year, current_month_code)) {
            // i. Set monthsBefore to monthsBefore + 1.
            ++months_before;
        }

        // c. If currentMonthCode is monthCode, then
        if (current_month_code == month_code) {
            // i. Return monthsBefore.
            return months_before;
        }

        // d. If isLeap is false, then
        //     i. Set isLeap to true.
        // e. Else,
        //     i. Set isLeap to false.
        //     ii. Set number to number + 1.
        if (exchange(is_leap, !is_leap))
            ++number;
    }

    VERIFY_NOT_REACHED();
}

// 4.1.8 CalendarDaysInMonth ( calendar, arithmeticYear, ordinalMonth ), https://tc39.es/proposal-intl-era-monthcode/#sec-temporal-calendardaysinmonth
u8 calendar_days_in_month(String const& calendar, i32 arithmetic_year, u8 ordinal_month)
{
    // 1. Let isoDate be ! CalendarIntegersToISO(calendar, arithmeticYear, ordinalMonth, 1).
    // 2. Return CalendarISOToDate(calendar, isoDate).[[DaysInMonth]].
    return Unicode::calendar_days_in_month(calendar, arithmetic_year, ordinal_month);
}

// 4.1.12 CalendarDateArithmeticYearForEraYear ( calendar, era, eraYear ), https://tc39.es/proposal-intl-era-monthcode/#sec-temporal-calendardatearithmeticyearforerayear
i32 calendar_date_arithmetic_year_for_era_year(String const& calendar, StringView era, i32 era_year)
{
    // 1. Let era be CanonicalizeEraInCalendar(calendar, era).
    // 2. Assert: era is not undefined.
    era = canonicalize_era_in_calendar(calendar, era).release_value();

    // 3. If calendar is not listed in the "Calendar Type" column of Table 1, return an implementation-defined value.
    // 4. Let r be the row in Table 2 with a value in the Calendar column matching calendar and a value in the Era
    //    column matching era.
    auto row = find_value(CALENDAR_ERA_DATA, [&](auto const& row) { return row.calendar == calendar && row.era == era; });
    if (!row.has_value())
        return era_year;

    // 5. Let eraKind be the value given in the "Era Kind" column of r.
    auto era_kind = row->kind;

    // 6. Let offset be the value given in the "Offset" column of r.
    auto offset = row->offset;

    switch (era_kind) {
    // 7. If eraKind is EPOCH, return eraYear.
    case CalendarEraData::Kind::Epoch:
        return era_year;

    // 8. If eraKind is NEGATIVE, return 1 - eraYear.
    case CalendarEraData::Kind::Negative:
        return 1 - era_year;

    // 9. Assert: eraKind is OFFSET.
    case CalendarEraData::Kind::Offset:
        // 10. Assert: offset is not undefined.
        VERIFY(offset.has_value());

        // 11. Return offset + eraYear - 1.
        return *offset + era_year - 1;
    }

    VERIFY_NOT_REACHED();
}

// 4.1.13 CalendarIntegersToISO ( calendar, arithmeticYear, ordinalMonth, day ), https://tc39.es/proposal-intl-era-monthcode/#sec-temporal-calendarintegerstoiso
ThrowCompletionOr<ISODate> calendar_integers_to_iso(VM& vm, String const& calendar, i32 arithmetic_year, u8 ordinal_month, u8 day)
{
    // 1. If arithmeticYear, ordinalMonth, and day do not form a valid date in calendar, throw a RangeError exception.
    // 2. Let isoDate be an ISO Date Record such that CalendarISOToDate(calendar, isoDate) returns a Calendar Date Record
    //    whose [[Year]], [[Month]], and [[Day]] field values respectively equal arithmeticYear, ordinalMonth, and day.
    // 3. NOTE: No known calendars have repeated dates that would cause isoDate to be ambiguous between two ISO Date Records.
    // 4. Return isoDate.
    if (auto iso_date = Unicode::calendar_date_to_iso_date(calendar, arithmetic_year, ordinal_month, day); iso_date.has_value())
        return create_iso_date_record(iso_date->year, iso_date->month, iso_date->day);
    return vm.throw_completion<RangeError>(ErrorType::TemporalInvalidISODate);
}

// 4.1.15 CalendarMonthsInYear ( calendar, arithmeticYear ), https://tc39.es/proposal-intl-era-monthcode/#sec-temporal-calendarmonthsinyear
u8 calendar_months_in_year(String const& calendar, i32 arithmetic_year)
{
    // 1. Let isoDate be ! CalendarIntegersToISO(calendar, arithmeticYear, 1, 1).
    // 2. Return CalendarISOToDate(calendar, isoDate).[[MonthsInYear]].
    return Unicode::calendar_months_in_year(calendar, arithmetic_year);
}

// 4.1.16 BalanceNonISODate ( calendar, arithmeticYear, ordinalMonth, day ), https://tc39.es/proposal-intl-era-monthcode/#sec-temporal-balancenonisodate
BalancedDate balance_non_iso_date(String const& calendar, i32 arithmetic_year, i32 ordinal_month, i32 day)
{
    // 1. Let resolvedYear be arithmeticYear.
    auto resolved_year = arithmetic_year;

    // 2. Let resolvedMonth be ordinalMonth.
    auto resolved_month = ordinal_month;

    // 3. Let monthsInYear be CalendarMonthsInYear(calendar, resolvedYear).
    auto months_in_year = calendar_months_in_year(calendar, resolved_year);

    // 4. Repeat, while resolvedMonth ≤ 0,
    while (resolved_month <= 0) {
        // a. Set resolvedYear to resolvedYear - 1.
        --resolved_year;

        // b. Set monthsInYear to CalendarMonthsInYear(calendar, resolvedYear).
        months_in_year = calendar_months_in_year(calendar, resolved_year);

        // c. Set resolvedMonth to resolvedMonth + monthsInYear.
        resolved_month += months_in_year;
    }

    // 5. Repeat, while resolvedMonth > monthsInYear,
    while (resolved_month > months_in_year) {
        // a. Set resolvedMonth to resolvedMonth - monthsInYear.
        resolved_month -= months_in_year;

        // b. Set resolvedYear to resolvedYear + 1.
        ++resolved_year;

        // c. Set monthsInYear to CalendarMonthsInYear(calendar, resolvedYear).
        months_in_year = calendar_months_in_year(calendar, resolved_year);
    }

    // 6. Let resolvedDay be day.
    auto resolved_day = day;

    // 7. Let daysInMonth be CalendarDaysInMonth(calendar, resolvedYear, resolvedMonth).
    auto days_in_month = calendar_days_in_month(calendar, resolved_year, resolved_month);

    // 8. Repeat, while resolvedDay ≤ 0,
    while (resolved_day <= 0) {
        // a. Set resolvedMonth to resolvedMonth - 1.
        --resolved_month;

        // b. If resolvedMonth is 0, then
        if (resolved_month == 0) {
            // i. Set resolvedYear to resolvedYear - 1.
            --resolved_year;

            // ii. Set monthsInYear to CalendarMonthsInYear(calendar, resolvedYear).
            months_in_year = calendar_months_in_year(calendar, resolved_year);

            // iii. Set resolvedMonth to monthsInYear.
            resolved_month = months_in_year;
        }

        // c. Set daysInMonth to CalendarDaysInMonth(calendar, resolvedYear, resolvedMonth).
        days_in_month = calendar_days_in_month(calendar, resolved_year, resolved_month);

        // d. Set resolvedDay to resolvedDay + daysInMonth.
        resolved_day += days_in_month;
    }

    // 9. Repeat, while resolvedDay > daysInMonth,
    while (resolved_day > days_in_month) {
        // a. Set resolvedDay to resolvedDay - daysInMonth.
        resolved_day -= days_in_month;

        // b. Set resolvedMonth to resolvedMonth + 1.
        ++resolved_month;

        // c. If resolvedMonth > monthsInYear, then
        if (resolved_month > months_in_year) {
            // i. Set resolvedYear to resolvedYear + 1.
            ++resolved_year;

            // ii. Set monthsInYear to CalendarMonthsInYear(calendar, resolvedYear).
            months_in_year = calendar_months_in_year(calendar, resolved_year);

            // iii. Set resolvedMonth to 1.
            resolved_month = 1;
        }

        // d. Set daysInMonth to CalendarDaysInMonth(calendar, resolvedYear, resolvedMonth).
        days_in_month = calendar_days_in_month(calendar, resolved_year, resolved_month);
    }

    // 10. Return the Record { [[Year]]: resolvedYear, [[Month]]: resolvedMonth, [[Day]]: resolvedDay }.
    return { .year = resolved_year, .month = static_cast<u8>(resolved_month), .day = static_cast<u8>(resolved_day) };
}

// 4.1.17 NonISODateSurpasses ( calendar, sign, fromIsoDate, toIsoDate, years, months, weeks, days ), https://tc39.es/proposal-intl-era-monthcode/#sec-temporal-nonisodatesurpasses
// NB: The only caller to this function is NonISODateUntil, which precomputes the calendar dates.
bool non_iso_date_surpasses(VM& vm, String const& calendar, i8 sign, CalendarDate const& from_calendar_date, CalendarDate const& to_calendar_date, double years, double months, double weeks, double days)
{
    // 1. Let parts be CalendarISOToDate(calendar, fromIsoDate).
    auto const& parts = from_calendar_date;

    // 2. Let calDate2 be CalendarISOToDate(calendar, toIsoDate).
    auto const& calendar_date_2 = to_calendar_date;

    // 3. Let y0 be parts.[[Year]] + years.
    auto y0 = parts.year + static_cast<i32>(years);

    // 4. If CompareSurpasses(sign, y0, parts.[[MonthCode]], parts.[[Day]], calDate2) is true, return true.
    if (compare_surpasses(sign, y0, parts.month_code, parts.day, calendar_date_2))
        return true;

    // 5. Let m0 be MonthCodeToOrdinal(calendar, y0, ! ConstrainMonthCode(calendar, y0, parts.[[MonthCode]], CONSTRAIN)).
    auto m0 = month_code_to_ordinal(calendar, y0, MUST(constrain_month_code(vm, calendar, y0, parts.month_code, Overflow::Constrain)));

    // 6. Let monthsAdded be BalanceNonISODate(calendar, y0, m0 + months, 1).
    auto months_added = balance_non_iso_date(calendar, y0, m0 + static_cast<i32>(months), 1);

    // 7. If CompareSurpasses(sign, monthsAdded.[[Year]], monthsAdded.[[Month]], parts.[[Day]], calDate2) is true, return true.
    if (compare_surpasses(sign, months_added.year, months_added.month, parts.day, calendar_date_2))
        return true;

    // 8. If weeks = 0 and days = 0, return false.
    if (weeks == 0 && days == 0)
        return false;

    // 9. Let endOfMonth be BalanceNonISODate(calendar, monthsAdded.[[Year]], monthsAdded.[[Month]] + 1, 0).
    auto end_of_month = balance_non_iso_date(calendar, months_added.year, months_added.month + 1, 0);

    // 10. Let baseDay be parts.[[Day]].
    auto base_day = parts.day;

    // 11. If baseDay ≤ endOfMonth.[[Day]], then
    //     a. Let regulatedDay be baseDay.
    // 12. Else,
    //     a. Let regulatedDay be endOfMonth.[[Day]].
    auto regulated_day = base_day <= end_of_month.day ? base_day : end_of_month.day;

    // 13. Let daysInWeek be 7 (the number of days in a week for all supported calendars).
    static constexpr auto days_in_week = 7;

    // 14. Let balancedDate be BalanceNonISODate(calendar, endOfMonth.[[Year]], endOfMonth.[[Month]], regulatedDay + daysInWeek * weeks + days).
    auto balanced_date = balance_non_iso_date(calendar, end_of_month.year, end_of_month.month, static_cast<i32>(regulated_day + (days_in_week * weeks) + days));

    // 15. Return CompareSurpasses(sign, balancedDate.[[Year]], balancedDate.[[Month]], balancedDate.[[Day]], calDate2).
    return compare_surpasses(sign, balanced_date.year, balanced_date.month, balanced_date.day, calendar_date_2);
}

}
