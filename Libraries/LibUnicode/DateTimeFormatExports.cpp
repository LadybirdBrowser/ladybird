/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibUnicode/DateTimeFormatExports.h>

#include <AK/NonnullOwnPtr.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibUnicode/DateTimeFormat.h>

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

template<typename Enum>
static Optional<Enum> optional_enum(u8 value, Enum last)
{
    if (value == UNICODE_CALENDAR_PATTERN_FIELD_ABSENT)
        return {};
    VERIFY(value <= to_underlying(last));
    return static_cast<Enum>(value);
}

template<typename Enum>
static u8 optional_enum_value(Optional<Enum> const& value)
{
    if (!value.has_value())
        return UNICODE_CALENDAR_PATTERN_FIELD_ABSENT;
    return to_underlying(*value);
}

static Optional<bool> optional_bool(u8 value)
{
    if (value == UNICODE_CALENDAR_PATTERN_FIELD_ABSENT)
        return {};
    VERIFY(value <= 1);
    return value == 1;
}

static Optional<Unicode::CalendarPatternStyle> optional_style(u8 value)
{
    return optional_enum(value, Unicode::CalendarPatternStyle::LongGeneric);
}

static Unicode::CalendarPattern calendar_pattern_from_fields(UnicodeCalendarPatternFields const& fields)
{
    Unicode::CalendarPattern pattern;
    pattern.hour_cycle = optional_enum(fields.hour_cycle, Unicode::HourCycle::H24);
    pattern.hour12 = optional_bool(fields.hour12);
    pattern.era = optional_style(fields.era);
    pattern.year = optional_style(fields.year);
    pattern.month = optional_style(fields.month);
    pattern.weekday = optional_style(fields.weekday);
    pattern.day = optional_style(fields.day);
    pattern.day_period = optional_style(fields.day_period);
    pattern.hour = optional_style(fields.hour);
    pattern.minute = optional_style(fields.minute);
    pattern.second = optional_style(fields.second);
    if (fields.fractional_second_digits != UNICODE_CALENDAR_PATTERN_FIELD_ABSENT)
        pattern.fractional_second_digits = fields.fractional_second_digits;
    pattern.time_zone_name = optional_style(fields.time_zone_name);
    return pattern;
}

static UnicodeCalendarPatternFields fields_of_calendar_pattern(Unicode::CalendarPattern const& pattern)
{
    UnicodeCalendarPatternFields fields;
    fields.hour_cycle = optional_enum_value(pattern.hour_cycle);
    fields.hour12 = pattern.hour12.has_value() ? static_cast<u8>(*pattern.hour12) : UNICODE_CALENDAR_PATTERN_FIELD_ABSENT;
    fields.era = optional_enum_value(pattern.era);
    fields.year = optional_enum_value(pattern.year);
    fields.month = optional_enum_value(pattern.month);
    fields.weekday = optional_enum_value(pattern.weekday);
    fields.day = optional_enum_value(pattern.day);
    fields.day_period = optional_enum_value(pattern.day_period);
    fields.hour = optional_enum_value(pattern.hour);
    fields.minute = optional_enum_value(pattern.minute);
    fields.second = optional_enum_value(pattern.second);
    fields.fractional_second_digits = pattern.fractional_second_digits.value_or(UNICODE_CALENDAR_PATTERN_FIELD_ABSENT);
    fields.time_zone_name = optional_enum_value(pattern.time_zone_name);
    return fields;
}

static Unicode::DateTimeFormat const& date_time_format_of(void const* date_time_format)
{
    VERIFY(date_time_format);
    return *static_cast<Unicode::DateTimeFormat const*>(date_time_format);
}

static void append_parts(Vector<Unicode::DateTimeFormat::Partition> const& parts, void* context, UnicodeAppendDateTimeFormatPart append_part)
{
    for (auto const& part : parts) {
        with_code_units(part.type.utf16_view(), [&](u16 const* type, size_t type_length) {
            with_code_units(part.value.utf16_view(), [&](u16 const* value, size_t value_length) {
                with_code_units(part.source.utf16_view(), [&](u16 const* source, size_t source_length) {
                    append_part(context, type, type_length, value, value_length, source, source_length);
                });
            });
        });
    }
}

extern "C" bool unicode_default_hour_cycle(UnicodeIntlText locale, u8* hour_cycle)
{
    auto default_hour_cycle = Unicode::default_hour_cycle(text_view(locale));
    if (!default_hour_cycle.has_value())
        return false;
    *hour_cycle = to_underlying(*default_hour_cycle);
    return true;
}

extern "C" void* unicode_date_time_format_create_for_date_and_time_style(UnicodeIntlText locale, UnicodeIntlText time_zone_identifier, u8 hour_cycle, u8 hour12, u8 date_style, u8 time_style)
{
    auto date_time_format = Unicode::DateTimeFormat::create_for_date_and_time_style(
        text_view(locale),
        text_view(time_zone_identifier),
        optional_enum(hour_cycle, Unicode::HourCycle::H24),
        optional_bool(hour12),
        optional_enum(date_style, Unicode::DateTimeStyle::Short),
        optional_enum(time_style, Unicode::DateTimeStyle::Short));
    return date_time_format.leak_ptr();
}

extern "C" void* unicode_date_time_format_create_for_pattern_options(UnicodeIntlText locale, UnicodeIntlText time_zone_identifier, UnicodeCalendarPatternFields options, bool has_pattern, UnicodeIntlText pattern)
{
    auto calendar_pattern = calendar_pattern_from_fields(options);
    if (has_pattern)
        calendar_pattern.pattern = MUST(text_view(pattern).to_utf8());

    auto date_time_format = Unicode::DateTimeFormat::create_for_pattern_options(text_view(locale), text_view(time_zone_identifier), calendar_pattern);
    return date_time_format.leak_ptr();
}

extern "C" void unicode_date_time_format_chosen_pattern(void const* date_time_format, UnicodeCalendarPatternFields* fields, UnicodeTextMappingOutput pattern)
{
    auto const& chosen_pattern = date_time_format_of(date_time_format).chosen_pattern();
    *fields = fields_of_calendar_pattern(chosen_pattern);

    VERIFY(chosen_pattern.pattern.has_value());
    auto pattern_string = Utf16String::from_utf8(*chosen_pattern.pattern);
    write_utf16_to(pattern, pattern_string.utf16_view());
}

extern "C" void unicode_date_time_format_format(void const* date_time_format, double time, UnicodeTextMappingOutput output)
{
    auto formatted = date_time_format_of(date_time_format).format(time);
    write_utf16_to(output, formatted.utf16_view());
}

extern "C" void unicode_date_time_format_format_to_parts(void const* date_time_format, double time, void* context, UnicodeAppendDateTimeFormatPart append_part)
{
    append_parts(date_time_format_of(date_time_format).format_to_parts(time), context, append_part);
}

extern "C" void unicode_date_time_format_format_range(void const* date_time_format, double start, double end, UnicodeTextMappingOutput output)
{
    auto formatted = date_time_format_of(date_time_format).format_range(start, end);
    write_utf16_to(output, formatted.utf16_view());
}

extern "C" void unicode_date_time_format_format_range_to_parts(void const* date_time_format, double start, double end, void* context, UnicodeAppendDateTimeFormatPart append_part)
{
    append_parts(date_time_format_of(date_time_format).format_range_to_parts(start, end), context, append_part);
}

extern "C" void unicode_date_time_format_destroy(void* date_time_format)
{
    VERIFY(date_time_format);
    delete static_cast<Unicode::DateTimeFormat*>(date_time_format);
}
