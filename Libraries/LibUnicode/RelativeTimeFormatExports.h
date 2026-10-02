/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibUnicode/IntlExports.h>
#include <LibUnicode/TextMapping.h>

// A part without a unit passes an empty unit.
using UnicodeAppendRelativeTimeFormatPart = void (*)(void* context, u16 const* type, size_t type_length, u16 const* value, size_t value_length, u16 const* unit, size_t unit_length);

extern "C" {
// style is a Unicode::Style, unit a Unicode::TimeUnit and numeric_display a Unicode::NumericDisplay.
void* unicode_relative_time_format_create(UnicodeIntlText locale, u8 style);
void unicode_relative_time_format_format(void const* relative_time_format, double value, u8 unit, u8 numeric_display, UnicodeTextMappingOutput);
void unicode_relative_time_format_format_to_parts(void const* relative_time_format, double value, u8 unit, u8 numeric_display, void* context, UnicodeAppendRelativeTimeFormatPart);
void unicode_relative_time_format_destroy(void* relative_time_format);
}
