/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Unicode {

enum class CaseMapping : u8 {
    Lowercase,
    Uppercase,
    Titlecase,
};

}

// The caller owns the destination and edit storage. All offsets are UTF-16 code units.
struct UnicodeTextMappingOutput {
    void* context;
    u16* (*allocate_text)(void*, size_t);
    void (*append_edit)(void*, size_t source_start, size_t source_length, size_t destination_start, size_t destination_length);
};

struct UnicodeISODate {
    i32 year;
    u8 month;
    u8 day;
};

struct UnicodeCalendarDate {
    bool has_era;
    bool has_era_year;
    i32 era_year;
    i32 year;
    u8 month;
    u8 day;
    u8 day_of_week;
    u16 day_of_year;
    bool has_week_of_year;
    u8 week_of_year;
    bool has_year_of_week;
    i32 year_of_week;
    u8 days_in_week;
    u8 days_in_month;
    u16 days_in_year;
    u8 months_in_year;
    bool in_leap_year;
};

extern "C" {
void unicode_apply_case_mapping(u16 const* text, size_t length, u8 mapping, u16 const* locale, size_t locale_length, bool preserve_existing, UnicodeTextMappingOutput);
void unicode_apply_fullwidth_mapping(u16 const* text, size_t length, UnicodeTextMappingOutput);
void unicode_normalize(u16 const* text, size_t length, u8 form, UnicodeTextMappingOutput);
bool unicode_text_may_require_bidi_processing(u16 const* text, size_t length);

void unicode_current_time_zone(UnicodeTextMappingOutput);
bool unicode_set_current_time_zone(u16 const* time_zone, size_t length);
void unicode_available_time_zones(void* context, void (*append)(void*, u16 const*, size_t));
bool unicode_resolve_primary_time_zone(u16 const* time_zone, size_t length, UnicodeTextMappingOutput);
bool unicode_time_zone_offset(u16 const* time_zone, size_t length, i64 seconds, u32 nanoseconds, i64* offset_nanoseconds, bool* in_dst);
size_t unicode_disambiguated_time_zone_offsets(u16 const* time_zone, size_t length, i64 seconds, u32 nanoseconds, i64* offset_nanoseconds, bool* in_dst, size_t capacity);
bool unicode_time_zone_transition(u16 const* time_zone, size_t length, i64 seconds, u32 nanoseconds, u8 direction, bool include_given_time, u8 transition_rule, i64* transition_milliseconds);
bool unicode_time_zone_display_name(u8 const* locale, size_t locale_length, u8 const* time_zone, size_t time_zone_length, bool in_dst, double time, UnicodeTextMappingOutput);
void unicode_default_locale(UnicodeTextMappingOutput);

bool unicode_parse_month_code(u16 const* month_code, size_t length, u8* month_number, bool* is_leap_month);
void unicode_create_month_code(u8 month_number, bool is_leap_month, UnicodeTextMappingOutput);
void unicode_iso_date_to_calendar_date(u8 const* calendar, size_t calendar_length, UnicodeISODate, UnicodeCalendarDate*, UnicodeTextMappingOutput era, UnicodeTextMappingOutput month_code);
bool unicode_calendar_date_to_iso_date(u8 const* calendar, size_t calendar_length, i32 year, u8 month, u8 day, UnicodeISODate*);
bool unicode_iso_year_and_month_code_to_iso_date(u8 const* calendar, size_t calendar_length, i32 year, u16 const* month_code, size_t month_code_length, u8 day, UnicodeISODate*);
bool unicode_calendar_year_and_month_code_to_iso_date(u8 const* calendar, size_t calendar_length, i32 arithmetic_year, u16 const* month_code, size_t month_code_length, u8 day, UnicodeISODate*);
u8 unicode_calendar_months_in_year(u8 const* calendar, size_t calendar_length, i32 arithmetic_year);
u8 unicode_calendar_days_in_month(u8 const* calendar, size_t calendar_length, i32 arithmetic_year, u8 ordinal_month);
u8 unicode_calendar_max_days_in_month_code(u8 const* calendar, size_t calendar_length, u16 const* month_code, size_t month_code_length);
bool unicode_calendar_year_contains_month_code(u8 const* calendar, size_t calendar_length, i32 arithmetic_year, u16 const* month_code, size_t month_code_length);
}
