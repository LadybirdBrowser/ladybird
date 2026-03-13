/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>

#include <unicode/calendar.h>

namespace Unicode {

// Custom icu::Calendar subclass to fix era handling for calendars where ICU's default behavior does not match ECMA-402
// requirements.
//
// Coptic calendar (SingleEra mode): ICU's Coptic calendar defines two eras (0 and 1), but the CLDR era 0 name is an
// empty string, causing formatToParts to omit the era entirely for pre-epoch dates. ECMA-402 treats Coptic as a
// single-era calendar where all dates use the same era ("Anno Martyrum") and the year can be negative. This subclass
// forces a fixed era value for all dates.
//
// Islamic calendars (DualEra mode): ICU's Islamic calendar implementations always set ERA=0 and allow the year to go
// negative for dates before the Hijra (622 CE). However, ICU's CLDR data defines two eras (era 0 "Anno Hegirae" and
// era 1 "Before Hijrah"), and ECMA-402 expects pre-epoch dates to use a distinct era. This subclass maps positive
// years to ERA=0 (AH) and non-positive years to ERA=1 (BH) with a positive year value.
class AdjustedEraCalendar final : public icu::Calendar {
public:
    enum class EraMode : u8 {
        SingleEra,
        DualEra,
    };

    AdjustedEraCalendar(NonnullOwnPtr<icu::Calendar> base_calendar, icu::Locale const&, UErrorCode&, EraMode);
    ~AdjustedEraCalendar() override;

    char const* getType() const override { return m_base_calendar->getType(); }

    AdjustedEraCalendar* clone() const override;

    UClassID getDynamicClassID() const override;
    static UClassID getStaticClassID();

private:
    AdjustedEraCalendar(AdjustedEraCalendar const&);

    void handleComputeFields(int32_t julian_day, UErrorCode&) override;
    int64_t handleComputeMonthStart(int32_t extended_year, int32_t month, UBool use_month, UErrorCode&) const override;
    int32_t handleGetExtendedYear(UErrorCode&) override;
    int32_t handleGetLimit(UCalendarDateFields field, ELimitType limit_type) const override;

    UBool haveDefaultCentury() const override { return m_base_calendar->haveDefaultCentury(); }
    UDate defaultCenturyStart() const override { return m_base_calendar->defaultCenturyStart(); }
    int32_t defaultCenturyStartYear() const override { return m_base_calendar->defaultCenturyStartYear(); }

    NonnullOwnPtr<icu::Calendar> m_base_calendar;
    EraMode m_era_mode;
};

}
