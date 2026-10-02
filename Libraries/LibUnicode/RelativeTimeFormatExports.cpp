/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibUnicode/RelativeTimeFormatExports.h>

#include <AK/NonnullOwnPtr.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibUnicode/Locale.h>
#include <LibUnicode/RelativeTimeFormat.h>

static Utf16View text_view(UnicodeIntlText text)
{
    if (text.ascii)
        return Utf16View { StringView { reinterpret_cast<char const*>(text.ascii), text.length } };
    if (text.utf16)
        return Utf16View { reinterpret_cast<char16_t const*>(text.utf16), text.length };
    VERIFY(text.length == 0);
    return {};
}

static void with_code_units(Utf16View text, auto&& callback)
{
    Vector<u16, 32> code_units;
    code_units.ensure_capacity(text.length_in_code_units());
    for (size_t i = 0; i < text.length_in_code_units(); ++i)
        code_units.unchecked_append(text.code_unit_at(i));
    callback(code_units.data(), code_units.size());
}

static void write_utf16_to(UnicodeTextMappingOutput output, Utf16View text)
{
    auto* destination = output.allocate_text(output.context, text.length_in_code_units());
    for (size_t i = 0; i < text.length_in_code_units(); ++i)
        destination[i] = text.code_unit_at(i);
}

static Unicode::RelativeTimeFormat const& relative_time_format_of(void const* relative_time_format)
{
    VERIFY(relative_time_format);
    return *static_cast<Unicode::RelativeTimeFormat const*>(relative_time_format);
}

static Unicode::TimeUnit time_unit_of(u8 unit)
{
    VERIFY(unit <= to_underlying(Unicode::TimeUnit::Year));
    return static_cast<Unicode::TimeUnit>(unit);
}

static Unicode::NumericDisplay numeric_display_of(u8 numeric_display)
{
    VERIFY(numeric_display <= to_underlying(Unicode::NumericDisplay::Auto));
    return static_cast<Unicode::NumericDisplay>(numeric_display);
}

extern "C" void* unicode_relative_time_format_create(UnicodeIntlText locale, u8 style)
{
    VERIFY(style <= to_underlying(Unicode::Style::Narrow));
    auto relative_time_format = Unicode::RelativeTimeFormat::create(text_view(locale), static_cast<Unicode::Style>(style));
    return relative_time_format.leak_ptr();
}

extern "C" void unicode_relative_time_format_format(void const* relative_time_format, double value, u8 unit, u8 numeric_display, UnicodeTextMappingOutput output)
{
    auto formatted = relative_time_format_of(relative_time_format).format(value, time_unit_of(unit), numeric_display_of(numeric_display));
    write_utf16_to(output, formatted.utf16_view());
}

extern "C" void unicode_relative_time_format_format_to_parts(void const* relative_time_format, double value, u8 unit, u8 numeric_display, void* context, UnicodeAppendRelativeTimeFormatPart append_part)
{
    auto parts = relative_time_format_of(relative_time_format).format_to_parts(value, time_unit_of(unit), numeric_display_of(numeric_display));
    for (auto const& part : parts) {
        with_code_units(part.type.utf16_view(), [&](u16 const* type, size_t type_length) {
            with_code_units(part.value.utf16_view(), [&](u16 const* value, size_t value_length) {
                with_code_units(part.unit.utf16_view(), [&](u16 const* unit, size_t unit_length) {
                    append_part(context, type, type_length, value, value_length, unit, unit_length);
                });
            });
        });
    }
}

extern "C" void unicode_relative_time_format_destroy(void* relative_time_format)
{
    VERIFY(relative_time_format);
    delete static_cast<Unicode::RelativeTimeFormat*>(relative_time_format);
}
