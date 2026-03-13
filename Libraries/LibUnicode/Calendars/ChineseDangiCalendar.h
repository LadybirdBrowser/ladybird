/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/String.h>

#include <unicode/calendar.h>

namespace Unicode {

// Custom icu::Calendar subclass for Chinese and Dangi lunisolar calendars that delegates calendar computation to icu4x.
// This ensures that DateTimeFormat uses the same calendar algorithms as Temporal.
class ChineseDangiCalendar final : public icu::Calendar {
public:
    ChineseDangiCalendar(NonnullOwnPtr<icu::Calendar> base_calendar, icu::Locale const&, UErrorCode&);
    ~ChineseDangiCalendar() override;

    char const* getType() const override { return m_base_calendar->getType(); }

    ChineseDangiCalendar* clone() const override;

    UClassID getDynamicClassID() const override;
    static UClassID getStaticClassID();

private:
    ChineseDangiCalendar(ChineseDangiCalendar const&);

    void handleComputeFields(int32_t julian_day, UErrorCode&) override;
    int64_t handleComputeMonthStart(int32_t extended_year, int32_t month, UBool use_month, UErrorCode&) const override;
    int32_t handleGetExtendedYear(UErrorCode&) override;
    int32_t handleGetLimit(UCalendarDateFields field, ELimitType limit_type) const override;

    int32_t internalGetMonth(int32_t default_value, UErrorCode&) const override;
    int32_t internalGetMonth(UErrorCode&) const override;

    bool inTemporalLeapYear(UErrorCode& status) const override;

    icu::UFieldResolutionTable const* getFieldResolutionTable() const override;
    UDate defaultCenturyStart() const override;
    int32_t defaultCenturyStartYear() const override;
    UBool haveDefaultCentury() const override;

    NonnullOwnPtr<icu::Calendar> m_base_calendar;
    String m_calendar_type;
};

}
