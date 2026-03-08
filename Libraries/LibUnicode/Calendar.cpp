/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <LibUnicode/Calendar.h>
#include <LibUnicode/ICU.h>

#include <unicode/calendar.h>

namespace Unicode {

static icu::Calendar& proleptic_gregorian_calendar()
{
    static auto calendar_data = CalendarData::for_calendar("gregory"_string);
    VERIFY(calendar_data.has_value());

    return calendar_data->calendar();
}

static void set_icu_calendar_to_iso_date(icu::Calendar& calendar, ISODate iso_date)
{
    UErrorCode status = U_ZERO_ERROR;

    auto& gregorian = proleptic_gregorian_calendar();
    gregorian.clear();
    gregorian.set(UCAL_EXTENDED_YEAR, iso_date.year);
    gregorian.set(UCAL_MONTH, iso_date.month - 1);
    gregorian.set(UCAL_DATE, iso_date.day);

    auto epoch_ms = ICU_MUST(gregorian.getTime(status));
    ICU_MUST_VOID(calendar.setTime(epoch_ms, status));
}

struct ChineseMonthEntry {
    u8 month { 0 };
    u8 days_in_month { 0 };
    i32 is_leap_month { 0 };
};

struct ChineseYearLayout {
    u8 month_count { 0 };
    Array<ChineseMonthEntry, 14> months; // This is a 1-based array to map to ICU ordinal months.
};

struct ChineseYearKey {
    String calendar;
    i32 extended_year { 0 };

    bool operator==(ChineseYearKey const& other) const = default;
};

struct ChineseExtendedYearKey {
    String calendar;
    i32 arithmetic_year { 0 };

    bool operator==(ChineseExtendedYearKey const&) const = default;
};

static HashMap<ChineseYearKey, ChineseYearLayout> s_chinese_year_layout_cache;
static HashMap<ChineseExtendedYearKey, i32> s_chinese_extended_year_cache;

static i32 chinese_arithmetic_year_to_extended_year(String const& calendar_name, icu::Calendar& calendar, i32 arithmetic_year)
{
    return s_chinese_extended_year_cache.ensure({ calendar_name, arithmetic_year }, [&]() {
        UErrorCode status = U_ZERO_ERROR;

        ISODate approximate_iso_date { arithmetic_year, 6, 15 };
        set_icu_calendar_to_iso_date(calendar, approximate_iso_date);

        return ICU_MUST(calendar.get(UCAL_EXTENDED_YEAR, status));
    });
}

static ChineseYearLayout const& chinese_year_layout(String const& calendar_name, icu::Calendar& calendar, i32 extended_year)
{
    return s_chinese_year_layout_cache.ensure({ calendar_name, extended_year }, [&]() {
        UErrorCode status = U_ZERO_ERROR;
        ChineseYearLayout layout;

        auto cloned = adopt_own(*calendar.clone());
        cloned->set(UCAL_MONTH, 0);
        cloned->set(UCAL_IS_LEAP_MONTH, 0);
        cloned->set(UCAL_DATE, 1);

        auto save_month_layout = [&]() {
            layout.months[++layout.month_count] = ChineseMonthEntry {
                .month = static_cast<u8>(ICU_MUST(cloned->get(UCAL_MONTH, status))),
                .days_in_month = static_cast<u8>(ICU_MUST(cloned->getActualMaximum(UCAL_DATE, status))),
                .is_leap_month = ICU_MUST(cloned->get(UCAL_IS_LEAP_MONTH, status)),
            };
        };

        auto start_year = ICU_MUST(cloned->get(UCAL_EXTENDED_YEAR, status));
        save_month_layout();

        for (int i = 0; i < 13; ++i) {
            cloned->add(UCAL_MONTH, 1, status);
            if (icu_failure(status))
                break;

            auto year = cloned->get(UCAL_EXTENDED_YEAR, status);
            if (icu_failure(status) || year != start_year)
                break;

            save_month_layout();
        }

        return layout;
    });
}

static ChineseYearLayout const& chinese_year_layout(String const& calendar_name, icu::Calendar& calendar)
{
    UErrorCode status = U_ZERO_ERROR;

    auto extended_year = ICU_MUST(calendar.get(UCAL_EXTENDED_YEAR, status));
    return chinese_year_layout(calendar_name, calendar, extended_year);
}

static bool is_hebrew_leap_year(icu::Calendar& calendar)
{
    UErrorCode status = U_ZERO_ERROR;

    auto cloned = adopt_own(*calendar.clone());
    cloned->set(UCAL_MONTH, HEBREW_ADAR_I_MONTH_NUMBER);
    cloned->set(UCAL_DATE, 1);

    auto month = ICU_MUST(cloned->get(UCAL_MONTH, status));
    return month == HEBREW_ADAR_I_MONTH_NUMBER;
}

static i32 hebrew_ordinal_month_to_icu_month(icu::Calendar& calendar, u8 ordinal_month)
{
    if (is_hebrew_leap_year(calendar))
        return ordinal_month - 1;
    return ordinal_month <= HEBREW_ADAR_I_MONTH_NUMBER ? ordinal_month - 1 : ordinal_month;
}

static u8 compute_ordinal_month(icu::Calendar& calendar, String const& calendar_name)
{
    UErrorCode status = U_ZERO_ERROR;

    auto month = static_cast<u8>(ICU_MUST(calendar.get(UCAL_MONTH, status)));

    if (calendar_name.is_one_of("chinese"sv, "dangi"sv)) {
        auto const& layout = chinese_year_layout(calendar_name, calendar);
        auto is_leap_month = ICU_MUST(calendar.get(UCAL_IS_LEAP_MONTH, status));

        for (u8 ordinal = 1; ordinal <= layout.month_count; ++ordinal) {
            if (layout.months[ordinal].month == month && layout.months[ordinal].is_leap_month == is_leap_month)
                return ordinal;
        }

        VERIFY_NOT_REACHED();
    }

    if (calendar_name == "hebrew"sv) {
        if (is_hebrew_leap_year(calendar))
            return month + 1;
        return month <= HEBREW_ADAR_I_MONTH_NUMBER ? month + 1 : month;
    }

    return month + 1;
}

struct ICUMonthCode {
    i32 month { 0 };
    i32 is_leap_month { 0 };
};
static ICUMonthCode month_code_to_icu_fields(StringView month_code, StringView calendar)
{
    auto month_number = month_code.substring_view(1, 2).to_number<u8>().value();
    bool is_leap_month = month_code.length() == 4 && month_code[3] == 'L';

    if (calendar == "hebrew"sv) {
        if (is_leap_month || month_number > HEBREW_ADAR_I_MONTH_NUMBER)
            return { month_number, is_leap_month };
        return { month_number - 1, 0 };
    }

    return { month_number - 1, is_leap_month };
}

static String compute_month_code(icu::Calendar& calendar, String const& calendar_name)
{
    UErrorCode status = U_ZERO_ERROR;

    auto month = static_cast<u8>(ICU_MUST(calendar.get(UCAL_MONTH, status)));
    auto is_leap_month = ICU_MUST(calendar.get(UCAL_IS_LEAP_MONTH, status));

    if (is_leap_month != 0) {
        if (calendar_name == "hebrew"sv)
            return String::from_utf8_without_validation(HEBREW_ADAR_I_MONTH_CODE.bytes());
        return MUST(String::formatted("M{:02}L", month + 1));
    }

    if (calendar_name == "hebrew"sv) {
        if (month < HEBREW_ADAR_I_MONTH_NUMBER)
            return MUST(String::formatted("M{:02}", month + 1));
        if (month == HEBREW_ADAR_I_MONTH_NUMBER && is_hebrew_leap_year(calendar))
            return String::from_utf8_without_validation(HEBREW_ADAR_I_MONTH_CODE.bytes());
        return MUST(String::formatted("M{:02}", month));
    }

    return MUST(String::formatted("M{:02}", month + 1));
}

static i32 arithmetic_year_to_extended_year(String const& calendar, i32 arithmetic_year)
{
    if (calendar == "buddhist"sv)
        return arithmetic_year + BUDDHIST_EPOCH_ISO_YEAR;
    if (calendar == "roc"sv)
        return arithmetic_year + ROC_EPOCH_ISO_YEAR;
    return arithmetic_year;
}

static i32 extended_year_to_arithmetic_year(String const& calendar, i32 extended_year)
{
    if (calendar == "buddhist"sv)
        return extended_year - BUDDHIST_EPOCH_ISO_YEAR;
    if (calendar == "roc"sv)
        return extended_year - ROC_EPOCH_ISO_YEAR;
    return extended_year;
}

static void set_calendar_to_arithmetic_year(icu::Calendar& calendar, String const& calendar_name, i32 arithmetic_year)
{
    auto extended_year = calendar_name.is_one_of("chinese"sv, "dangi"sv)
        ? chinese_arithmetic_year_to_extended_year(calendar_name, calendar, arithmetic_year)
        : arithmetic_year_to_extended_year(calendar_name, arithmetic_year);

    calendar.set(UCAL_EXTENDED_YEAR, extended_year);
}

CalendarDate iso_date_to_calendar_date(String const& calendar, ISODate iso_date)
{
    UErrorCode status = U_ZERO_ERROR;

    auto calendar_data = CalendarData::for_calendar(calendar);
    if (!calendar_data.has_value())
        return {};

    auto& icu_calendar = calendar_data->calendar();
    set_icu_calendar_to_iso_date(icu_calendar, iso_date);

    i32 arithmetic_year = 0;
    if (calendar.is_one_of("chinese"sv, "dangi"sv)) {
        // For Chinese/Dangi calendars, the arithmetic year is the ISO year of the start of the current calendar year.
        auto year_start = adopt_own(*icu_calendar.clone());
        year_start->set(UCAL_MONTH, 0);
        year_start->set(UCAL_DATE, 1);

        auto epoch_ms = ICU_MUST(year_start->getTime(status));

        auto& gregorian = proleptic_gregorian_calendar();
        ICU_MUST_VOID(gregorian.setTime(epoch_ms, status));

        arithmetic_year = ICU_MUST(gregorian.get(UCAL_EXTENDED_YEAR, status));
    } else {
        auto extended_year = ICU_MUST(icu_calendar.get(UCAL_EXTENDED_YEAR, status));
        arithmetic_year = extended_year_to_arithmetic_year(calendar, extended_year);
    }

    auto ordinal_month = compute_ordinal_month(icu_calendar, calendar);
    auto month_code = compute_month_code(icu_calendar, calendar);
    auto day = static_cast<u8>(ICU_MUST(icu_calendar.get(UCAL_DATE, status)));

    // In ICU, the days of the week begin with Sunday=1. Temporal expects Monday=1 and Sunday=7 instead.
    auto day_of_week = static_cast<u8>(ICU_MUST(icu_calendar.get(UCAL_DAY_OF_WEEK, status)));
    day_of_week = day_of_week == UCAL_SUNDAY ? 7 : day_of_week - 1;

    auto day_of_year = static_cast<u16>(ICU_MUST(icu_calendar.get(UCAL_DAY_OF_YEAR, status)));
    auto days_in_month = static_cast<u8>(ICU_MUST(icu_calendar.getActualMaximum(UCAL_DATE, status)));
    auto days_in_year = static_cast<u16>(ICU_MUST(icu_calendar.getActualMaximum(UCAL_DAY_OF_YEAR, status)));
    auto months_in_year = calendar_months_in_year(calendar, arithmetic_year);
    auto in_leap_year = ICU_MUST(icu_calendar.inTemporalLeapYear(status));

    return CalendarDate {
        .era = {},
        .era_year = {},
        .year = arithmetic_year,
        .month = ordinal_month,
        .month_code = move(month_code),
        .day = day,
        .day_of_week = day_of_week,
        .day_of_year = day_of_year,
        .week_of_year = {},
        .days_in_week = 7,
        .days_in_month = days_in_month,
        .days_in_year = days_in_year,
        .months_in_year = months_in_year,
        .in_leap_year = in_leap_year,
    };
}

Optional<ISODate> calendar_date_to_iso_date(String const& calendar, i32 arithmetic_year, u8 ordinal_month, u8 day)
{
    UErrorCode status = U_ZERO_ERROR;

    auto calendar_data = CalendarData::for_calendar(calendar);
    if (!calendar_data.has_value())
        return {};

    auto& icu_calendar = calendar_data->calendar();
    icu_calendar.clear();

    set_calendar_to_arithmetic_year(icu_calendar, calendar, arithmetic_year);

    if (calendar.is_one_of("chinese"sv, "dangi"sv)) {
        auto const& layout = chinese_year_layout(calendar, icu_calendar);

        if (ordinal_month >= 1 && ordinal_month <= layout.month_count) {
            icu_calendar.set(UCAL_MONTH, layout.months[ordinal_month].month);
            icu_calendar.set(UCAL_IS_LEAP_MONTH, layout.months[ordinal_month].is_leap_month);
        }

        icu_calendar.set(UCAL_DATE, day);
    } else if (calendar == "hebrew"sv) {
        auto icu_month = hebrew_ordinal_month_to_icu_month(icu_calendar, ordinal_month);
        icu_calendar.set(UCAL_MONTH, icu_month);
        icu_calendar.set(UCAL_DATE, day);
    } else {
        icu_calendar.set(UCAL_MONTH, ordinal_month - 1);
        icu_calendar.set(UCAL_DATE, day);
    }

    icu_calendar.set(UCAL_HOUR_OF_DAY, 12);

    auto epoch_ms = ICU_MUST(icu_calendar.getTime(status));

    auto& gregorian = proleptic_gregorian_calendar();
    ICU_MUST_VOID(gregorian.setTime(epoch_ms, status));

    auto iso_year = ICU_MUST(gregorian.get(UCAL_EXTENDED_YEAR, status));
    auto iso_month = ICU_MUST(gregorian.get(UCAL_MONTH, status)) + 1;
    auto iso_day = ICU_MUST(gregorian.get(UCAL_DATE, status));

    return ISODate { iso_year, static_cast<u8>(iso_month), static_cast<u8>(iso_day) };
}

Optional<ISODate> calendar_month_code_to_iso_date(String const& calendar, i32 year, StringView month_code, u8 day)
{
    UErrorCode status = U_ZERO_ERROR;

    auto calendar_data = CalendarData::for_calendar(calendar);
    if (!calendar_data.has_value())
        return {};

    auto& icu_calendar = calendar_data->calendar();
    auto& gregorian = proleptic_gregorian_calendar();

    set_icu_calendar_to_iso_date(icu_calendar, { year, 1, 1 });
    auto start_extended_year = ICU_MUST(icu_calendar.get(UCAL_EXTENDED_YEAR, status));

    set_icu_calendar_to_iso_date(icu_calendar, { year, 12, 31 });
    auto end_extended_year = ICU_MUST(icu_calendar.get(UCAL_EXTENDED_YEAR, status));

    auto [icu_month, icu_is_leap_month] = month_code_to_icu_fields(month_code, calendar);
    auto is_chinese_or_dangi = calendar.is_one_of("chinese"sv, "dangi"sv);

    Optional<ISODate> best_iso_date;

    for (auto extended_year = start_extended_year; extended_year <= end_extended_year; ++extended_year) {
        icu_calendar.clear();
        icu_calendar.set(UCAL_EXTENDED_YEAR, extended_year);
        icu_calendar.set(UCAL_MONTH, icu_month);
        if (is_chinese_or_dangi)
            icu_calendar.set(UCAL_IS_LEAP_MONTH, icu_is_leap_month);
        icu_calendar.set(UCAL_DATE, day);
        icu_calendar.set(UCAL_HOUR_OF_DAY, 12);

        auto epoch_ms = ICU_MUST(icu_calendar.getTime(status));
        ICU_MUST_VOID(gregorian.setTime(epoch_ms, status));

        auto resolved_month = ICU_MUST(icu_calendar.get(UCAL_MONTH, status));
        if (resolved_month != icu_month)
            continue;

        if (is_chinese_or_dangi) {
            auto resolved_is_leap_month = ICU_MUST(icu_calendar.get(UCAL_IS_LEAP_MONTH, status));
            if (resolved_is_leap_month != icu_is_leap_month)
                continue;
        }

        auto resolved_day = ICU_MUST(icu_calendar.get(UCAL_DATE, status));
        if (resolved_day != day)
            continue;

        auto iso_year = static_cast<i32>(ICU_MUST(gregorian.get(UCAL_EXTENDED_YEAR, status)));
        if (iso_year != year)
            continue;

        auto iso_month = static_cast<u8>(ICU_MUST(gregorian.get(UCAL_MONTH, status)) + 1);
        auto iso_day = static_cast<u8>(ICU_MUST(gregorian.get(UCAL_DATE, status)));

        if (!best_iso_date.has_value() || iso_month > best_iso_date->month || (iso_month == best_iso_date->month && iso_day > best_iso_date->day))
            best_iso_date = ISODate { iso_year, iso_month, iso_day };
    }

    return best_iso_date;
}

u8 calendar_months_in_year(String const& calendar, i32 arithmetic_year)
{
    UErrorCode status = U_ZERO_ERROR;

    auto calendar_data = CalendarData::for_calendar(calendar);
    if (!calendar_data.has_value())
        return 12;

    auto& icu_calendar = calendar_data->calendar();

    if (calendar.is_one_of("chinese"sv, "dangi"sv)) {
        auto extended_year = chinese_arithmetic_year_to_extended_year(calendar, icu_calendar, arithmetic_year);
        return chinese_year_layout(calendar, icu_calendar, extended_year).month_count;
    }

    icu_calendar.clear();
    set_calendar_to_arithmetic_year(icu_calendar, calendar, arithmetic_year);
    icu_calendar.set(UCAL_MONTH, 0);
    icu_calendar.set(UCAL_IS_LEAP_MONTH, 0);
    icu_calendar.set(UCAL_DATE, 1);

    if (calendar == "hebrew"sv)
        return is_hebrew_leap_year(icu_calendar) ? 13 : 12;

    return static_cast<u8>(ICU_MUST(icu_calendar.getActualMaximum(UCAL_MONTH, status)) + 1);
}

u8 calendar_days_in_month(String const& calendar, i32 arithmetic_year, u8 ordinal_month)
{
    UErrorCode status = U_ZERO_ERROR;

    auto calendar_data = CalendarData::for_calendar(calendar);
    if (!calendar_data.has_value())
        return 30;

    auto& icu_calendar = calendar_data->calendar();

    if (calendar.is_one_of("chinese"sv, "dangi"sv)) {
        auto extended_year = chinese_arithmetic_year_to_extended_year(calendar, icu_calendar, arithmetic_year);
        auto const& layout = chinese_year_layout(calendar, icu_calendar, extended_year);

        if (ordinal_month >= 1 && ordinal_month <= layout.month_count)
            return layout.months[ordinal_month].days_in_month;

        return 30;
    }

    icu_calendar.clear();
    set_calendar_to_arithmetic_year(icu_calendar, calendar, arithmetic_year);

    if (calendar == "hebrew"sv) {
        auto icu_month = hebrew_ordinal_month_to_icu_month(icu_calendar, ordinal_month);
        icu_calendar.set(UCAL_MONTH, icu_month);
    } else {
        icu_calendar.set(UCAL_MONTH, ordinal_month - 1);
    }

    icu_calendar.set(UCAL_DATE, 1);

    return static_cast<u8>(ICU_MUST(icu_calendar.getActualMaximum(UCAL_DATE, status)));
}

u8 calendar_max_days_in_month_code(String const& calendar, StringView month_code)
{
    UErrorCode status = U_ZERO_ERROR;

    auto calendar_data = CalendarData::for_calendar(calendar);
    if (!calendar_data.has_value())
        return 30;

    auto& icu_calendar = calendar_data->calendar();

    set_icu_calendar_to_iso_date(icu_calendar, { 1970, 7, 1 });
    auto base_extended_year = ICU_MUST(icu_calendar.get(UCAL_EXTENDED_YEAR, status));

    auto [icu_month, icu_is_leap] = month_code_to_icu_fields(month_code, calendar);
    u8 max_days_in_month = 0;

    for (auto offset = -2; offset <= 2; ++offset) {
        icu_calendar.clear();
        icu_calendar.set(UCAL_EXTENDED_YEAR, base_extended_year + offset);
        icu_calendar.set(UCAL_MONTH, icu_month);
        icu_calendar.set(UCAL_DATE, 1);

        if (icu_is_leap != 0 || (calendar == "hebrew"sv && month_code == HEBREW_ADAR_I_MONTH_CODE)) {
            auto resolved = ICU_MUST(icu_calendar.get(UCAL_MONTH, status));
            if (resolved != icu_month)
                continue;
        }

        auto days_in_month = static_cast<u8>(ICU_MUST(icu_calendar.getActualMaximum(UCAL_DATE, status)));
        max_days_in_month = max(max_days_in_month, days_in_month);
    }

    return max_days_in_month > 0 ? max_days_in_month : 30;
}

Optional<MonthCode> chinese_ordinal_month_code(String const& calendar, i32 arithmetic_year, u8 ordinal_month)
{
    ASSERT(calendar.is_one_of("chinese"sv, "dangi"sv));

    auto calendar_data = CalendarData::for_calendar(calendar);
    if (!calendar_data.has_value())
        return {};

    auto& icu_calendar = calendar_data->calendar();

    auto extended_year = chinese_arithmetic_year_to_extended_year(calendar, icu_calendar, arithmetic_year);
    auto const& layout = chinese_year_layout(calendar, icu_calendar, extended_year);

    if (ordinal_month < 1 || ordinal_month > layout.month_count)
        return {};

    auto const& entry = layout.months[ordinal_month];
    return MonthCode { static_cast<u8>(entry.month + 1), entry.is_leap_month != 0 };
}

}

namespace AK {

template<>
struct Traits<Unicode::ChineseYearKey> : public DefaultTraits<Unicode::ChineseYearKey> {
    static unsigned hash(Unicode::ChineseYearKey const& key) { return pair_int_hash(key.calendar.hash(), key.extended_year); }
};

template<>
struct Traits<Unicode::ChineseExtendedYearKey> : public DefaultTraits<Unicode::ChineseExtendedYearKey> {
    static unsigned hash(Unicode::ChineseExtendedYearKey const& key) { return pair_int_hash(key.calendar.hash(), key.arithmetic_year); }
};

}
