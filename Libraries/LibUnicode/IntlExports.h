/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibUnicode/TextMapping.h>

// A borrowed UTF-16 string in either of AK::Utf16View's storages: `ascii` is set when the string has ASCII storage,
// and `utf16` otherwise. Lengths are in UTF-16 code units.
struct UnicodeIntlText {
    u8 const* ascii;
    u16 const* utf16;
    size_t length;
};

// The parts of a Unicode::LocaleID, in the order parse_unicode_locale_id() parsed them. A transformed extension's
// language follows its TransformedLanguage part as Root, Language, Script, Region and Variant parts of its own.
enum class UnicodeLocaleIDPart : u8 {
    Root,
    Language,
    Script,
    Region,
    Variant,
    LocaleExtension,
    LocaleExtensionAttribute,
    LocaleExtensionKeywordKey,
    LocaleExtensionKeywordValue,
    TransformedExtension,
    TransformedLanguage,
    TransformedFieldKey,
    TransformedFieldValue,
    OtherExtensionKey,
    OtherExtensionValue,
    PrivateUseExtension,
};

using UnicodeAppendString = void (*)(void* context, u16 const* text, size_t length);
using UnicodeAppendLocaleIDPart = void (*)(void* context, u8 part, u16 const* text, size_t length);
using UnicodeAppendListFormatPart = void (*)(void* context, u16 const* type, size_t type_length, u16 const* value, size_t value_length);

extern "C" {
bool unicode_parse_unicode_locale_id(UnicodeIntlText locale, void* context, UnicodeAppendLocaleIDPart append_part);
bool unicode_is_unicode_language_id(UnicodeIntlText language);
bool unicode_is_type_identifier(UnicodeIntlText identifier);
bool unicode_canonicalize_unicode_locale_id(UnicodeIntlText locale, UnicodeTextMappingOutput);
void unicode_canonicalize_unicode_extension_values(UnicodeIntlText key, UnicodeIntlText value, UnicodeTextMappingOutput);
bool unicode_is_locale_available(UnicodeIntlText locale);
bool unicode_add_likely_subtags(UnicodeIntlText locale, UnicodeTextMappingOutput);
bool unicode_remove_likely_subtags(UnicodeIntlText locale, UnicodeTextMappingOutput);
bool unicode_is_locale_character_ordering_right_to_left(UnicodeIntlText locale);

void unicode_available_keyword_values(UnicodeIntlText locale, UnicodeIntlText key, void* context, UnicodeAppendString);
void unicode_available_calendars(void* context, UnicodeAppendString);
void unicode_available_calendars_of_locale(UnicodeIntlText locale, void* context, UnicodeAppendString);
void unicode_available_currencies(void* context, UnicodeAppendString);
void unicode_available_collations(void* context, UnicodeAppendString);
void unicode_available_collations_of_locale(UnicodeIntlText locale, void* context, UnicodeAppendString);
void unicode_available_hour_cycles_of_locale(UnicodeIntlText locale, void* context, UnicodeAppendString);
void unicode_available_number_systems(void* context, UnicodeAppendString);
void unicode_available_number_systems_of_locale(UnicodeIntlText locale, void* context, UnicodeAppendString);
void unicode_available_time_zones_in_region(UnicodeIntlText region, void* context, UnicodeAppendString);

// Writes up to seven Unicode::Weekday values to weekend_days.
void unicode_week_info_of_locale(UnicodeIntlText locale, bool* has_first_day_of_week, u8* first_day_of_week, u8* weekend_days, size_t* weekend_day_count);

void* unicode_collator_create(UnicodeIntlText locale, u8 usage, UnicodeIntlText collation, bool has_sensitivity, u8 sensitivity, u8 case_first, bool numeric, bool has_ignore_punctuation, bool ignore_punctuation);
u8 unicode_collator_compare(void const* collator, UnicodeIntlText lhs, UnicodeIntlText rhs);
u8 unicode_collator_sensitivity(void const* collator);
bool unicode_collator_ignore_punctuation(void const* collator);
void unicode_collator_destroy(void* collator);

void* unicode_list_format_create(UnicodeIntlText locale, u8 type, u8 style);
void unicode_list_format_format(void const* list_format, UnicodeIntlText const* list, size_t list_size, UnicodeTextMappingOutput);
void unicode_list_format_format_to_parts(void const* list_format, UnicodeIntlText const* list, size_t list_size, void* context, UnicodeAppendListFormatPart);
void unicode_list_format_destroy(void* list_format);

bool unicode_language_display_name(UnicodeIntlText locale, UnicodeIntlText language, u8 language_display, UnicodeTextMappingOutput);
bool unicode_region_display_name(UnicodeIntlText locale, UnicodeIntlText region, UnicodeTextMappingOutput);
bool unicode_script_display_name(UnicodeIntlText locale, UnicodeIntlText script, UnicodeTextMappingOutput);
bool unicode_currency_display_name(UnicodeIntlText locale, UnicodeIntlText currency, u8 style, UnicodeTextMappingOutput);
bool unicode_calendar_display_name(UnicodeIntlText locale, UnicodeIntlText calendar, UnicodeTextMappingOutput);
bool unicode_date_time_field_display_name(UnicodeIntlText locale, UnicodeIntlText field, u8 style, UnicodeTextMappingOutput);

void* unicode_segmenter_create(UnicodeIntlText locale, u8 granularity);
void* unicode_segmenter_clone(void const* segmenter);
void unicode_segmenter_set_segmented_text(void* segmenter, UnicodeIntlText text);
size_t unicode_segmenter_current_boundary(void* segmenter);
bool unicode_segmenter_previous_boundary(void* segmenter, size_t index, bool inclusive, size_t* boundary);
bool unicode_segmenter_next_boundary(void* segmenter, size_t index, bool inclusive, size_t* boundary);
bool unicode_segmenter_is_current_boundary_word_like(void const* segmenter);
void unicode_segmenter_destroy(void* segmenter);
}
