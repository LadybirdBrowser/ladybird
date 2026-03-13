/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibUnicode/Calendars/AdjustedEraCalendar.h>
#include <LibUnicode/Calendars/Constants.h>
#include <LibUnicode/ICU.h>

namespace Unicode {

AdjustedEraCalendar::AdjustedEraCalendar(NonnullOwnPtr<icu::Calendar> base_calendar, icu::Locale const& locale, UErrorCode& status, EraMode era_mode)
    : icu::Calendar(base_calendar->getTimeZone(), locale, status)
    , m_base_calendar(move(base_calendar))
    , m_era_mode(era_mode)
{
}

AdjustedEraCalendar::~AdjustedEraCalendar() = default;

AdjustedEraCalendar::AdjustedEraCalendar(AdjustedEraCalendar const& other)
    : icu::Calendar(other)
    , m_base_calendar(adopt_own(*other.m_base_calendar->clone()))
    , m_era_mode(other.m_era_mode)
{
}

AdjustedEraCalendar* AdjustedEraCalendar::clone() const
{
    return new AdjustedEraCalendar(*this);
}

void AdjustedEraCalendar::handleComputeFields(int32_t julian_day, UErrorCode& status)
{
    if (icu_failure(status))
        return;

    auto time = static_cast<UDate>(static_cast<i64>(julian_day) - EPOCH_START_AS_JULIAN_DAY) * U_MILLIS_PER_DAY;

    m_base_calendar->setTime(time, status);
    if (icu_failure(status))
        return;

    auto extended_year = m_base_calendar->get(UCAL_EXTENDED_YEAR, status);
    auto month = m_base_calendar->get(UCAL_MONTH, status);
    auto ordinal_month = m_base_calendar->get(UCAL_ORDINAL_MONTH, status);
    auto day_of_month = m_base_calendar->get(UCAL_DAY_OF_MONTH, status);
    auto day_of_year = m_base_calendar->get(UCAL_DAY_OF_YEAR, status);
    if (icu_failure(status))
        return;

    i32 era = 0;
    i32 display_year = 0;

    switch (m_era_mode) {
    case EraMode::SingleEra:
        era = 1;
        display_year = extended_year;
        break;

    case EraMode::DualEra:
        if (extended_year > 0) {
            era = 0;
            display_year = extended_year;
        } else {
            era = 1;
            display_year = 1 - extended_year;
        }
        break;
    }

    internalSet(UCAL_ERA, era);
    internalSet(UCAL_YEAR, display_year);
    internalSet(UCAL_EXTENDED_YEAR, extended_year);
    internalSet(UCAL_MONTH, month);
    internalSet(UCAL_ORDINAL_MONTH, ordinal_month);
    internalSet(UCAL_DAY_OF_MONTH, day_of_month);
    internalSet(UCAL_DAY_OF_YEAR, day_of_year);
}

int64_t AdjustedEraCalendar::handleComputeMonthStart(int32_t extended_year, int32_t month, UBool use_month, UErrorCode& status) const
{
    if (icu_failure(status))
        return 0;

    auto& base_calendar = const_cast<icu::Calendar&>(*m_base_calendar);
    base_calendar.clear();

    base_calendar.set(UCAL_EXTENDED_YEAR, extended_year);
    base_calendar.set(use_month ? UCAL_MONTH : UCAL_ORDINAL_MONTH, month);
    base_calendar.set(UCAL_DAY_OF_MONTH, 1);

    auto time = base_calendar.getTime(status);
    if (icu_failure(status))
        return 0;

    // handleComputeMonthStart must return the Julian day of the day BEFORE month start.
    return EPOCH_START_AS_JULIAN_DAY + static_cast<int64_t>(time / U_MILLIS_PER_DAY) - 1;
}

int32_t AdjustedEraCalendar::handleGetExtendedYear(UErrorCode& status)
{
    if (icu_failure(status))
        return 0;

    if (newerField(UCAL_EXTENDED_YEAR, UCAL_YEAR) == UCAL_EXTENDED_YEAR)
        return internalGet(UCAL_EXTENDED_YEAR, 1);

    auto era = internalGet(UCAL_ERA);
    auto year = internalGet(UCAL_YEAR, 1);

    switch (m_era_mode) {
    case EraMode::SingleEra:
        return year;
    case EraMode::DualEra:
        return era == 0 ? year : 1 - year;
    }

    VERIFY_NOT_REACHED();
}

int32_t AdjustedEraCalendar::handleGetLimit(UCalendarDateFields field, ELimitType limit_type) const
{
    if (field == UCAL_ERA) {
        switch (m_era_mode) {
        case EraMode::SingleEra:
            return 1;
        case EraMode::DualEra:
            return limit_type == UCAL_LIMIT_MINIMUM || limit_type == UCAL_LIMIT_GREATEST_MINIMUM ? 0 : 1;
        }
    }

    switch (limit_type) {
    case UCAL_LIMIT_MINIMUM:
        return m_base_calendar->getMinimum(field);
    case UCAL_LIMIT_GREATEST_MINIMUM:
        return m_base_calendar->getGreatestMinimum(field);
    case UCAL_LIMIT_LEAST_MAXIMUM:
        return m_base_calendar->getLeastMaximum(field);
    case UCAL_LIMIT_MAXIMUM:
        return m_base_calendar->getMaximum(field);
    default:
        VERIFY_NOT_REACHED();
    }
}

UOBJECT_DEFINE_RTTI_IMPLEMENTATION(AdjustedEraCalendar)

}
