/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibUnicode/Calendar.h>
#include <LibUnicode/Calendars/ChineseDangiCalendar.h>
#include <LibUnicode/Calendars/Constants.h>
#include <LibUnicode/ICU.h>

namespace Unicode {

// This is a bit weird, but lets us delegate otherwise protected methods to the original icu4c calendar. The static_cast
// is safe here because these are virtual methods dispatched through the vtable.
struct CalendarAccessor : icu::Calendar {
    using Calendar::getFieldResolutionTable;
    using Calendar::handleGetLimit;
};
static CalendarAccessor const& calendar_accessor(icu::Calendar const& calendar)
{
    return static_cast<CalendarAccessor const&>(calendar);
}

ChineseDangiCalendar::ChineseDangiCalendar(NonnullOwnPtr<icu::Calendar> base_calendar, icu::Locale const& locale, UErrorCode& status)
    : icu::Calendar(base_calendar->getTimeZone(), locale, status)
    , m_base_calendar(move(base_calendar))
{
    StringView calendar_type { m_base_calendar->getType(), strlen(m_base_calendar->getType()) };
    m_calendar_type = MUST(String::from_utf8(calendar_type));
}

ChineseDangiCalendar::~ChineseDangiCalendar() = default;

ChineseDangiCalendar::ChineseDangiCalendar(ChineseDangiCalendar const& other)
    : icu::Calendar(other)
    , m_base_calendar(adopt_own(*other.m_base_calendar->clone()))
    , m_calendar_type(other.m_calendar_type)
{
}

ChineseDangiCalendar* ChineseDangiCalendar::clone() const
{
    return new ChineseDangiCalendar(*this);
}

void ChineseDangiCalendar::handleComputeFields(int32_t, UErrorCode& status)
{
    if (icu_failure(status))
        return;

    auto iso_year = getGregorianYear();
    auto iso_month = static_cast<u8>(getGregorianMonth() + 1);
    auto iso_day = static_cast<u8>(getGregorianDayOfMonth());

    auto calendar_date = iso_date_to_calendar_date(m_calendar_type, ISODate { iso_year, iso_month, iso_day });
    auto month_code = parse_month_code(calendar_date.month_code);
    if (!month_code.has_value()) {
        status = U_INTERNAL_PROGRAM_ERROR;
        return;
    }

    // Compute the 60-year cycle and year-of cycle.
    auto calendar_year = calendar_date.year - (m_calendar_type == "chinese"sv ? CHINESE_CALENDAR_FIRST_YEAR : DANGI_CALENDAR_FIRST_YEAR);
    auto cycle_year = calendar_year - 1;
    auto cycle = (cycle_year / 60) - (cycle_year % 60 < 0 ? 1 : 0);
    auto year_of_cycle = cycle_year - (cycle * 60);

    internalSet(UCAL_ERA, cycle + 1);
    internalSet(UCAL_YEAR, year_of_cycle + 1);
    internalSet(UCAL_EXTENDED_YEAR, calendar_date.year);
    internalSet(UCAL_MONTH, month_code->month_number - 1);
    internalSet(UCAL_ORDINAL_MONTH, calendar_date.month - 1);
    internalSet(UCAL_IS_LEAP_MONTH, month_code->is_leap_month ? 1 : 0);
    internalSet(UCAL_DAY_OF_MONTH, calendar_date.day);
    internalSet(UCAL_DAY_OF_YEAR, calendar_date.day_of_year);
}

int64_t ChineseDangiCalendar::handleComputeMonthStart(int32_t extended_year, int32_t month, UBool use_month, UErrorCode& status) const
{
    if (icu_failure(status))
        return 0;

    if (month < 0 || month > (use_month ? 11 : 12)) {
        status = U_ILLEGAL_ARGUMENT_ERROR;
        return 0;
    }

    Optional<ISODate> iso_date;

    if (use_month) {
        bool is_leap_month = internalGet(UCAL_IS_LEAP_MONTH) != 0;
        auto month_code = create_month_code(month + 1, is_leap_month);
        iso_date = calendar_year_and_month_code_to_iso_date(m_calendar_type, extended_year, month_code, 1);
    } else {
        iso_date = calendar_date_to_iso_date(m_calendar_type, extended_year, static_cast<u8>(month + 1), 1);
    }

    if (!iso_date.has_value()) {
        status = U_INTERNAL_PROGRAM_ERROR;
        return 0;
    }

    // handleComputeMonthStart must return the Julian day of the day BEFORE month start.
    return EPOCH_START_AS_JULIAN_DAY + days_since_epoch(iso_date->year, iso_date->month, iso_date->day) - 1;
}

int32_t ChineseDangiCalendar::handleGetExtendedYear(UErrorCode& status)
{
    if (icu_failure(status))
        return 0;

    if (newerField(UCAL_EXTENDED_YEAR, UCAL_YEAR) == UCAL_EXTENDED_YEAR)
        return internalGet(UCAL_EXTENDED_YEAR, 1);

    status = U_UNSUPPORTED_ERROR;
    return 0;
}

int32_t ChineseDangiCalendar::handleGetLimit(UCalendarDateFields field, ELimitType limit_type) const
{
    return calendar_accessor(*m_base_calendar).handleGetLimit(field, limit_type);
}

int32_t ChineseDangiCalendar::internalGetMonth(int32_t default_value, UErrorCode& status) const
{
    if (icu_failure(status))
        return 0;

    if (resolveFields(kMonthPrecedence) == UCAL_MONTH)
        return internalGet(UCAL_MONTH, default_value);
    return internalGetMonth(status);
}

int32_t ChineseDangiCalendar::internalGetMonth(UErrorCode& status) const
{
    if (icu_failure(status))
        return 0;

    if (resolveFields(kMonthPrecedence) == UCAL_MONTH)
        return internalGet(UCAL_MONTH);

    auto extended_year = internalGet(UCAL_EXTENDED_YEAR);
    auto ordinal_month = internalGet(UCAL_ORDINAL_MONTH);

    auto iso_date = calendar_date_to_iso_date(m_calendar_type, extended_year, static_cast<u8>(ordinal_month + 1), 1);
    if (!iso_date.has_value()) {
        status = U_INTERNAL_PROGRAM_ERROR;
        return 0;
    }

    auto calendar_date = iso_date_to_calendar_date(m_calendar_type, *iso_date);
    auto month_code = parse_month_code(calendar_date.month_code);
    if (!month_code.has_value()) {
        status = U_INTERNAL_PROGRAM_ERROR;
        return 0;
    }

    auto month = month_code->month_number - 1;

    auto* self = const_cast<ChineseDangiCalendar*>(this);
    self->internalSet(UCAL_IS_LEAP_MONTH, month_code->is_leap_month);
    self->internalSet(UCAL_MONTH, month);

    return month;
}

bool ChineseDangiCalendar::inTemporalLeapYear(UErrorCode& status) const
{
    return m_base_calendar->inTemporalLeapYear(status);
}

icu::UFieldResolutionTable const* ChineseDangiCalendar::getFieldResolutionTable() const
{
    return calendar_accessor(*m_base_calendar).getFieldResolutionTable();
}

UDate ChineseDangiCalendar::defaultCenturyStart() const
{
    return m_base_calendar->defaultCenturyStart();
}

int32_t ChineseDangiCalendar::defaultCenturyStartYear() const
{
    return m_base_calendar->defaultCenturyStartYear();
}

UBool ChineseDangiCalendar::haveDefaultCentury() const
{
    return m_base_calendar->haveDefaultCentury();
}

UOBJECT_DEFINE_RTTI_IMPLEMENTATION(ChineseDangiCalendar)

}
