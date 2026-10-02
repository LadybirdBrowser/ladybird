/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibUnicode/IntlExports.h>

// A Unicode::NumberFormat::Value: the decimal string `string` when `is_string` is set, and `number` otherwise.
struct UnicodeNumberFormatValue {
    bool is_string;
    double number;
    UnicodeIntlText string;
};

// Unicode::DisplayOptions. Each has_ field says whether the Optional member it precedes is set. The enumerations are
// the underlying values of Unicode::NumberFormatStyle, SignDisplay, Notation, CompactDisplay, Grouping,
// CurrencyDisplay, CurrencySign and Style.
struct UnicodeNumberFormatDisplayOptions {
    u8 style;
    u8 sign_display;
    u8 notation;
    bool has_compact_display;
    u8 compact_display;
    u8 grouping;
    bool has_currency;
    UnicodeIntlText currency;
    bool has_currency_display;
    u8 currency_display;
    bool has_currency_sign;
    u8 currency_sign;
    bool has_unit;
    UnicodeIntlText unit;
    bool has_unit_display;
    u8 unit_display;
};

// Unicode::RoundingOptions, with the underlying values of Unicode::RoundingType, RoundingMode and TrailingZeroDisplay.
struct UnicodeNumberFormatRoundingOptions {
    u8 type;
    u8 mode;
    u8 trailing_zero_display;
    bool has_min_significant_digits;
    i32 min_significant_digits;
    bool has_max_significant_digits;
    i32 max_significant_digits;
    bool has_min_fraction_digits;
    i32 min_fraction_digits;
    bool has_max_fraction_digits;
    i32 max_fraction_digits;
    i32 min_integer_digits;
    i32 rounding_increment;
};

// Receives one Unicode::NumberFormat::Partition. The source is empty for the partitions of a single number.
using UnicodeAppendNumberFormatPart = void (*)(void* context, u16 const* type, size_t type_length, u16 const* value, size_t value_length, u16 const* source, size_t source_length);
using UnicodeAppendPluralCategory = void (*)(void* context, u8 category);

extern "C" {
void* unicode_number_format_create(UnicodeIntlText locale, UnicodeNumberFormatDisplayOptions const* display_options, UnicodeNumberFormatRoundingOptions const* rounding_options);
void unicode_number_format_format(void const* number_format, UnicodeNumberFormatValue value, UnicodeTextMappingOutput);
void unicode_number_format_format_to_parts(void const* number_format, UnicodeNumberFormatValue value, void* context, UnicodeAppendNumberFormatPart);
void unicode_number_format_format_range(void const* number_format, UnicodeNumberFormatValue start, UnicodeNumberFormatValue end, UnicodeTextMappingOutput);
void unicode_number_format_format_range_to_parts(void const* number_format, UnicodeNumberFormatValue start, UnicodeNumberFormatValue end, void* context, UnicodeAppendNumberFormatPart);
void unicode_number_format_create_plural_rules(void* number_format, u8 plural_form);
u8 unicode_number_format_select_plural(void const* number_format, UnicodeNumberFormatValue value);
u8 unicode_number_format_select_plural_range(void const* number_format, UnicodeNumberFormatValue start, UnicodeNumberFormatValue end);
void unicode_number_format_available_plural_categories(void const* number_format, void* context, UnicodeAppendPluralCategory);
void unicode_number_format_destroy(void* number_format);

// Unicode::get_currency_code: whether the currency is in the list, and if so, its minor unit if it has one.
bool unicode_get_currency_code(UnicodeIntlText currency, bool* has_minor_unit, i32* minor_unit);

// Unicode::digital_format.
void unicode_digital_format(UnicodeIntlText locale, UnicodeTextMappingOutput hours_minutes_separator, UnicodeTextMappingOutput minutes_seconds_separator, bool* uses_two_digit_hours);
}
