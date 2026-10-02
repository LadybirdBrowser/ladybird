/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibUnicode/NumberFormatExports.h>

#include <AK/NonnullOwnPtr.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibUnicode/CurrencyCode.h>
#include <LibUnicode/DurationFormat.h>
#include <LibUnicode/Locale.h>
#include <LibUnicode/NumberFormat.h>
#include <LibUnicode/PluralRules.h>

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

static Unicode::NumberFormat::Value number_format_value(UnicodeNumberFormatValue value)
{
    if (value.is_string)
        return Utf16String::from_utf16(text_view(value.string));
    return value.number;
}

static Unicode::DisplayOptions display_options_from(UnicodeNumberFormatDisplayOptions const& options)
{
    VERIFY(options.style <= to_underlying(Unicode::NumberFormatStyle::Unit));
    VERIFY(options.sign_display <= to_underlying(Unicode::SignDisplay::Negative));
    VERIFY(options.notation <= to_underlying(Unicode::Notation::Compact));
    VERIFY(options.compact_display <= to_underlying(Unicode::CompactDisplay::Long));
    VERIFY(options.grouping <= to_underlying(Unicode::Grouping::False));
    VERIFY(options.currency_display <= to_underlying(Unicode::CurrencyDisplay::Name));
    VERIFY(options.currency_sign <= to_underlying(Unicode::CurrencySign::Accounting));
    VERIFY(options.unit_display <= to_underlying(Unicode::Style::Narrow));

    Unicode::DisplayOptions display_options;
    display_options.style = static_cast<Unicode::NumberFormatStyle>(options.style);
    display_options.sign_display = static_cast<Unicode::SignDisplay>(options.sign_display);
    display_options.notation = static_cast<Unicode::Notation>(options.notation);
    if (options.has_compact_display)
        display_options.compact_display = static_cast<Unicode::CompactDisplay>(options.compact_display);
    display_options.grouping = static_cast<Unicode::Grouping>(options.grouping);
    if (options.has_currency)
        display_options.currency = Utf16String::from_utf16(text_view(options.currency));
    if (options.has_currency_display)
        display_options.currency_display = static_cast<Unicode::CurrencyDisplay>(options.currency_display);
    if (options.has_currency_sign)
        display_options.currency_sign = static_cast<Unicode::CurrencySign>(options.currency_sign);
    if (options.has_unit)
        display_options.unit = Utf16String::from_utf16(text_view(options.unit));
    if (options.has_unit_display)
        display_options.unit_display = static_cast<Unicode::Style>(options.unit_display);
    return display_options;
}

static Unicode::RoundingOptions rounding_options_from(UnicodeNumberFormatRoundingOptions const& options)
{
    VERIFY(options.type <= to_underlying(Unicode::RoundingType::LessPrecision));
    VERIFY(options.mode <= to_underlying(Unicode::RoundingMode::Trunc));
    VERIFY(options.trailing_zero_display <= to_underlying(Unicode::TrailingZeroDisplay::StripIfInteger));

    Unicode::RoundingOptions rounding_options;
    rounding_options.type = static_cast<Unicode::RoundingType>(options.type);
    rounding_options.mode = static_cast<Unicode::RoundingMode>(options.mode);
    rounding_options.trailing_zero_display = static_cast<Unicode::TrailingZeroDisplay>(options.trailing_zero_display);
    if (options.has_min_significant_digits)
        rounding_options.min_significant_digits = options.min_significant_digits;
    if (options.has_max_significant_digits)
        rounding_options.max_significant_digits = options.max_significant_digits;
    if (options.has_min_fraction_digits)
        rounding_options.min_fraction_digits = options.min_fraction_digits;
    if (options.has_max_fraction_digits)
        rounding_options.max_fraction_digits = options.max_fraction_digits;
    rounding_options.min_integer_digits = options.min_integer_digits;
    rounding_options.rounding_increment = options.rounding_increment;
    return rounding_options;
}

static void append_partitions(Vector<Unicode::NumberFormat::Partition> const& partitions, void* context, UnicodeAppendNumberFormatPart append_part)
{
    for (auto const& partition : partitions) {
        with_code_units(partition.type.utf16_view(), [&](u16 const* type, size_t type_length) {
            with_code_units(partition.value.utf16_view(), [&](u16 const* value, size_t value_length) {
                with_code_units(partition.source.utf16_view(), [&](u16 const* source, size_t source_length) {
                    append_part(context, type, type_length, value, value_length, source, source_length);
                });
            });
        });
    }
}

static Unicode::NumberFormat const& number_format_from(void const* number_format)
{
    VERIFY(number_format);
    return *static_cast<Unicode::NumberFormat const*>(number_format);
}

extern "C" void* unicode_number_format_create(UnicodeIntlText locale, UnicodeNumberFormatDisplayOptions const* display_options, UnicodeNumberFormatRoundingOptions const* rounding_options)
{
    VERIFY(display_options);
    VERIFY(rounding_options);

    auto number_format = Unicode::NumberFormat::create(
        text_view(locale),
        display_options_from(*display_options),
        rounding_options_from(*rounding_options));
    return number_format.leak_ptr();
}

extern "C" void unicode_number_format_format(void const* number_format, UnicodeNumberFormatValue value, UnicodeTextMappingOutput output)
{
    auto formatted = number_format_from(number_format).format(number_format_value(value));
    write_utf16_to(output, formatted.utf16_view());
}

extern "C" void unicode_number_format_format_to_parts(void const* number_format, UnicodeNumberFormatValue value, void* context, UnicodeAppendNumberFormatPart append_part)
{
    auto partitions = number_format_from(number_format).format_to_parts(number_format_value(value));
    append_partitions(partitions, context, append_part);
}

extern "C" void unicode_number_format_format_range(void const* number_format, UnicodeNumberFormatValue start, UnicodeNumberFormatValue end, UnicodeTextMappingOutput output)
{
    auto formatted = number_format_from(number_format).format_range(number_format_value(start), number_format_value(end));
    write_utf16_to(output, formatted.utf16_view());
}

extern "C" void unicode_number_format_format_range_to_parts(void const* number_format, UnicodeNumberFormatValue start, UnicodeNumberFormatValue end, void* context, UnicodeAppendNumberFormatPart append_part)
{
    auto partitions = number_format_from(number_format).format_range_to_parts(number_format_value(start), number_format_value(end));
    append_partitions(partitions, context, append_part);
}

extern "C" void unicode_number_format_create_plural_rules(void* number_format, u8 plural_form)
{
    VERIFY(number_format);
    VERIFY(plural_form <= to_underlying(Unicode::PluralForm::Ordinal));
    static_cast<Unicode::NumberFormat*>(number_format)->create_plural_rules(static_cast<Unicode::PluralForm>(plural_form));
}

extern "C" u8 unicode_number_format_select_plural(void const* number_format, UnicodeNumberFormatValue value)
{
    return to_underlying(number_format_from(number_format).select_plural(number_format_value(value)));
}

extern "C" u8 unicode_number_format_select_plural_range(void const* number_format, UnicodeNumberFormatValue start, UnicodeNumberFormatValue end)
{
    return to_underlying(number_format_from(number_format).select_plural_range(number_format_value(start), number_format_value(end)));
}

extern "C" void unicode_number_format_available_plural_categories(void const* number_format, void* context, UnicodeAppendPluralCategory append_category)
{
    for (auto category : number_format_from(number_format).available_plural_categories())
        append_category(context, to_underlying(category));
}

extern "C" void unicode_number_format_destroy(void* number_format)
{
    VERIFY(number_format);
    delete static_cast<Unicode::NumberFormat*>(number_format);
}

extern "C" bool unicode_get_currency_code(UnicodeIntlText currency, bool* has_minor_unit, i32* minor_unit)
{
    auto currency_code = Unicode::get_currency_code(text_view(currency).bytes());
    if (!currency_code.has_value())
        return false;

    *has_minor_unit = currency_code->minor_unit.has_value();
    *minor_unit = currency_code->minor_unit.value_or(0);
    return true;
}

extern "C" void unicode_digital_format(UnicodeIntlText locale, UnicodeTextMappingOutput hours_minutes_separator, UnicodeTextMappingOutput minutes_seconds_separator, bool* uses_two_digit_hours)
{
    auto digital_format = Unicode::digital_format(text_view(locale));
    write_utf16_to(hours_minutes_separator, digital_format.hours_minutes_separator.utf16_view());
    write_utf16_to(minutes_seconds_separator, digital_format.minutes_seconds_separator.utf16_view());
    *uses_two_digit_hours = digital_format.uses_two_digit_hours;
}
