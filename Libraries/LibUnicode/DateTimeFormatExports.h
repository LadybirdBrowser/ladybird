/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibUnicode/IntlExports.h>
#include <LibUnicode/TextMapping.h>

// The value of a field of UnicodeCalendarPatternFields that the Unicode::CalendarPattern does not have.
static constexpr u8 UNICODE_CALENDAR_PATTERN_FIELD_ABSENT = 0xff;

// A Unicode::CalendarPattern without its pattern string. hour_cycle holds a Unicode::HourCycle, hour12 is 0 or 1,
// fractional_second_digits is the number of digits, and every other field holds a Unicode::CalendarPatternStyle; any
// of them is UNICODE_CALENDAR_PATTERN_FIELD_ABSENT when the pattern does not have it.
struct UnicodeCalendarPatternFields {
    u8 hour_cycle;
    u8 hour12;
    u8 era;
    u8 year;
    u8 month;
    u8 weekday;
    u8 day;
    u8 day_period;
    u8 hour;
    u8 minute;
    u8 second;
    u8 fractional_second_digits;
    u8 time_zone_name;
};

using UnicodeAppendDateTimeFormatPart = void (*)(void* context, u16 const* type, size_t type_length, u16 const* value, size_t value_length, u16 const* source, size_t source_length);

extern "C" {
// Writes a Unicode::HourCycle to hour_cycle, and returns false when the locale has no default hour cycle.
bool unicode_default_hour_cycle(UnicodeIntlText locale, u8* hour_cycle);

// hour_cycle, hour12, date_style and time_style are UNICODE_CALENDAR_PATTERN_FIELD_ABSENT when they are not given.
void* unicode_date_time_format_create_for_date_and_time_style(UnicodeIntlText locale, UnicodeIntlText time_zone_identifier, u8 hour_cycle, u8 hour12, u8 date_style, u8 time_style);
void* unicode_date_time_format_create_for_pattern_options(UnicodeIntlText locale, UnicodeIntlText time_zone_identifier, UnicodeCalendarPatternFields options, bool has_pattern, UnicodeIntlText pattern);

// Writes the fields of the chosen pattern to fields, and its pattern string to pattern.
void unicode_date_time_format_chosen_pattern(void const* date_time_format, UnicodeCalendarPatternFields* fields, UnicodeTextMappingOutput pattern);

void unicode_date_time_format_format(void const* date_time_format, double time, UnicodeTextMappingOutput);
void unicode_date_time_format_format_to_parts(void const* date_time_format, double time, void* context, UnicodeAppendDateTimeFormatPart);
void unicode_date_time_format_format_range(void const* date_time_format, double start, double end, UnicodeTextMappingOutput);
void unicode_date_time_format_format_range_to_parts(void const* date_time_format, double start, double end, void* context, UnicodeAppendDateTimeFormatPart);
void unicode_date_time_format_destroy(void* date_time_format);
}
