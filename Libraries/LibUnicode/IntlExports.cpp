/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibUnicode/IntlExports.h>

#include <AK/ByteString.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibUnicode/Collator.h>
#include <LibUnicode/DateTimeFormat.h>
#include <LibUnicode/DisplayNames.h>
#include <LibUnicode/ListFormat.h>
#include <LibUnicode/Locale.h>
#include <LibUnicode/Segmenter.h>
#include <LibUnicode/TimeZone.h>
#include <LibUnicode/UnicodeKeywords.h>

static Utf16View text_view(UnicodeIntlText text)
{
    if (text.ascii)
        return Utf16View { StringView { reinterpret_cast<char const*>(text.ascii), text.length } };
    if (text.utf16)
        return Utf16View { reinterpret_cast<char16_t const*>(text.utf16), text.length };
    VERIFY(text.length == 0);
    return {};
}

static ByteString ascii_byte_string(UnicodeIntlText text)
{
    auto view = text_view(text);
    VERIFY(view.is_ascii());
    return MUST(view.to_byte_string());
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

static void append_strings(Vector<Utf16String> const& strings, void* context, UnicodeAppendString append)
{
    for (auto const& string : strings) {
        with_code_units(string.utf16_view(), [&](u16 const* code_units, size_t length) {
            append(context, code_units, length);
        });
    }
}

static void append_locale_id_part(void* context, UnicodeAppendLocaleIDPart append_part, UnicodeLocaleIDPart part, Utf16View text)
{
    with_code_units(text, [&](u16 const* code_units, size_t length) {
        append_part(context, to_underlying(part), code_units, length);
    });
}

static void append_language_id_parts(Unicode::LanguageID const& language_id, void* context, UnicodeAppendLocaleIDPart append_part)
{
    if (language_id.is_root)
        append_locale_id_part(context, append_part, UnicodeLocaleIDPart::Root, {});
    if (language_id.language.has_value())
        append_locale_id_part(context, append_part, UnicodeLocaleIDPart::Language, language_id.language->utf16_view());
    if (language_id.script.has_value())
        append_locale_id_part(context, append_part, UnicodeLocaleIDPart::Script, language_id.script->utf16_view());
    if (language_id.region.has_value())
        append_locale_id_part(context, append_part, UnicodeLocaleIDPart::Region, language_id.region->utf16_view());
    for (auto const& variant : language_id.variants)
        append_locale_id_part(context, append_part, UnicodeLocaleIDPart::Variant, variant.utf16_view());
}

extern "C" bool unicode_parse_unicode_locale_id(UnicodeIntlText locale, void* context, UnicodeAppendLocaleIDPart append_part)
{
    auto locale_id = Unicode::parse_unicode_locale_id(text_view(locale));
    if (!locale_id.has_value())
        return false;

    append_language_id_parts(locale_id->language_id, context, append_part);

    for (auto const& extension : locale_id->extensions) {
        extension.visit(
            [&](Unicode::LocaleExtension const& locale_extension) {
                append_locale_id_part(context, append_part, UnicodeLocaleIDPart::LocaleExtension, {});
                for (auto const& attribute : locale_extension.attributes)
                    append_locale_id_part(context, append_part, UnicodeLocaleIDPart::LocaleExtensionAttribute, attribute.utf16_view());
                for (auto const& keyword : locale_extension.keywords) {
                    append_locale_id_part(context, append_part, UnicodeLocaleIDPart::LocaleExtensionKeywordKey, keyword.key.utf16_view());
                    append_locale_id_part(context, append_part, UnicodeLocaleIDPart::LocaleExtensionKeywordValue, keyword.value.utf16_view());
                }
            },
            [&](Unicode::TransformedExtension const& transformed_extension) {
                append_locale_id_part(context, append_part, UnicodeLocaleIDPart::TransformedExtension, {});
                if (transformed_extension.language.has_value()) {
                    append_locale_id_part(context, append_part, UnicodeLocaleIDPart::TransformedLanguage, {});
                    append_language_id_parts(*transformed_extension.language, context, append_part);
                }
                for (auto const& field : transformed_extension.fields) {
                    append_locale_id_part(context, append_part, UnicodeLocaleIDPart::TransformedFieldKey, field.key.utf16_view());
                    append_locale_id_part(context, append_part, UnicodeLocaleIDPart::TransformedFieldValue, field.value.utf16_view());
                }
            },
            [&](Unicode::OtherExtension const& other_extension) {
                char key[] = { other_extension.key };
                append_locale_id_part(context, append_part, UnicodeLocaleIDPart::OtherExtensionKey, Utf16View { StringView { key, 1 } });
                append_locale_id_part(context, append_part, UnicodeLocaleIDPart::OtherExtensionValue, other_extension.value.utf16_view());
            });
    }

    for (auto const& private_use_extension : locale_id->private_use_extensions)
        append_locale_id_part(context, append_part, UnicodeLocaleIDPart::PrivateUseExtension, private_use_extension.utf16_view());

    return true;
}

extern "C" bool unicode_is_unicode_language_id(UnicodeIntlText language)
{
    return Unicode::parse_unicode_language_id(text_view(language)).has_value();
}

extern "C" bool unicode_is_type_identifier(UnicodeIntlText identifier)
{
    return Unicode::is_type_identifier(text_view(identifier));
}

extern "C" bool unicode_canonicalize_unicode_locale_id(UnicodeIntlText locale, UnicodeTextMappingOutput output)
{
    auto canonicalized = Unicode::canonicalize_unicode_locale_id(text_view(locale));
    if (!canonicalized.has_value())
        return false;
    write_utf16_to(output, canonicalized->utf16_view());
    return true;
}

extern "C" void unicode_canonicalize_unicode_extension_values(UnicodeIntlText key, UnicodeIntlText value, UnicodeTextMappingOutput output)
{
    auto canonicalized = Unicode::canonicalize_unicode_extension_values(StringView { text_view(key).bytes() }, text_view(value));
    write_utf16_to(output, canonicalized.utf16_view());
}

extern "C" bool unicode_is_locale_available(UnicodeIntlText locale)
{
    return Unicode::is_locale_available(StringView { text_view(locale).bytes() });
}

extern "C" bool unicode_add_likely_subtags(UnicodeIntlText locale, UnicodeTextMappingOutput output)
{
    auto maximal = Unicode::add_likely_subtags(text_view(locale));
    if (!maximal.has_value())
        return false;
    write_utf16_to(output, maximal->utf16_view());
    return true;
}

extern "C" bool unicode_remove_likely_subtags(UnicodeIntlText locale, UnicodeTextMappingOutput output)
{
    auto minimal = Unicode::remove_likely_subtags(text_view(locale));
    if (!minimal.has_value())
        return false;
    write_utf16_to(output, minimal->utf16_view());
    return true;
}

extern "C" bool unicode_is_locale_character_ordering_right_to_left(UnicodeIntlText locale)
{
    return Unicode::is_locale_character_ordering_right_to_left(text_view(locale));
}

extern "C" void unicode_available_keyword_values(UnicodeIntlText locale, UnicodeIntlText key, void* context, UnicodeAppendString append)
{
    append_strings(Unicode::available_keyword_values(text_view(locale), text_view(key)), context, append);
}

extern "C" void unicode_available_calendars(void* context, UnicodeAppendString append)
{
    append_strings(Unicode::available_calendars(), context, append);
}

extern "C" void unicode_available_calendars_of_locale(UnicodeIntlText locale, void* context, UnicodeAppendString append)
{
    append_strings(Unicode::available_calendars(text_view(locale)), context, append);
}

extern "C" void unicode_available_currencies(void* context, UnicodeAppendString append)
{
    append_strings(Unicode::available_currencies(), context, append);
}

extern "C" void unicode_available_collations(void* context, UnicodeAppendString append)
{
    append_strings(Unicode::available_collations(), context, append);
}

extern "C" void unicode_available_collations_of_locale(UnicodeIntlText locale, void* context, UnicodeAppendString append)
{
    append_strings(Unicode::available_collations(text_view(locale)), context, append);
}

extern "C" void unicode_available_hour_cycles_of_locale(UnicodeIntlText locale, void* context, UnicodeAppendString append)
{
    append_strings(Unicode::available_hour_cycles(text_view(locale)), context, append);
}

extern "C" void unicode_available_number_systems(void* context, UnicodeAppendString append)
{
    append_strings(Unicode::available_number_systems(), context, append);
}

extern "C" void unicode_available_number_systems_of_locale(UnicodeIntlText locale, void* context, UnicodeAppendString append)
{
    append_strings(Unicode::available_number_systems(text_view(locale)), context, append);
}

extern "C" void unicode_available_time_zones_in_region(UnicodeIntlText region, void* context, UnicodeAppendString append)
{
    append_strings(Unicode::available_time_zones_in_region(text_view(region)), context, append);
}

extern "C" void unicode_week_info_of_locale(UnicodeIntlText locale, bool* has_first_day_of_week, u8* first_day_of_week, u8* weekend_days, size_t* weekend_day_count)
{
    auto week_info = Unicode::week_info_of_locale(text_view(locale));

    *has_first_day_of_week = week_info.first_day_of_week.has_value();
    *first_day_of_week = to_underlying(week_info.first_day_of_week.value_or(Unicode::Weekday::Sunday));

    VERIFY(week_info.weekend_days.size() <= 7);
    for (size_t i = 0; i < week_info.weekend_days.size(); ++i)
        weekend_days[i] = to_underlying(week_info.weekend_days[i]);
    *weekend_day_count = week_info.weekend_days.size();
}

extern "C" void* unicode_collator_create(UnicodeIntlText locale, u8 usage, UnicodeIntlText collation, bool has_sensitivity, u8 sensitivity, u8 case_first, bool numeric, bool has_ignore_punctuation, bool ignore_punctuation)
{
    VERIFY(usage <= to_underlying(Unicode::Usage::Search));
    VERIFY(sensitivity <= to_underlying(Unicode::Sensitivity::Variant));
    VERIFY(case_first <= to_underlying(Unicode::CaseFirst::False));

    auto collator = Unicode::Collator::create(
        text_view(locale),
        static_cast<Unicode::Usage>(usage),
        text_view(collation),
        has_sensitivity ? Optional<Unicode::Sensitivity> { static_cast<Unicode::Sensitivity>(sensitivity) } : OptionalNone {},
        static_cast<Unicode::CaseFirst>(case_first),
        numeric,
        has_ignore_punctuation ? Optional<bool> { ignore_punctuation } : OptionalNone {});
    return collator.leak_ptr();
}

extern "C" u8 unicode_collator_compare(void const* collator, UnicodeIntlText lhs, UnicodeIntlText rhs)
{
    VERIFY(collator);
    return to_underlying(static_cast<Unicode::Collator const*>(collator)->compare(text_view(lhs), text_view(rhs)));
}

extern "C" u8 unicode_collator_sensitivity(void const* collator)
{
    VERIFY(collator);
    return to_underlying(static_cast<Unicode::Collator const*>(collator)->sensitivity());
}

extern "C" bool unicode_collator_ignore_punctuation(void const* collator)
{
    VERIFY(collator);
    return static_cast<Unicode::Collator const*>(collator)->ignore_punctuation();
}

extern "C" void unicode_collator_destroy(void* collator)
{
    VERIFY(collator);
    delete static_cast<Unicode::Collator*>(collator);
}

static Vector<Utf16String> string_list(UnicodeIntlText const* list, size_t list_size)
{
    Vector<Utf16String> strings;
    strings.ensure_capacity(list_size);
    for (size_t i = 0; i < list_size; ++i)
        strings.unchecked_append(Utf16String::from_utf16(text_view(list[i])));
    return strings;
}

extern "C" void* unicode_list_format_create(UnicodeIntlText locale, u8 type, u8 style)
{
    VERIFY(type <= to_underlying(Unicode::ListFormatType::Unit));
    VERIFY(style <= to_underlying(Unicode::Style::Narrow));

    auto list_format = Unicode::ListFormat::create(text_view(locale), static_cast<Unicode::ListFormatType>(type), static_cast<Unicode::Style>(style));
    return list_format.leak_ptr();
}

extern "C" void unicode_list_format_format(void const* list_format, UnicodeIntlText const* list, size_t list_size, UnicodeTextMappingOutput output)
{
    VERIFY(list_format);
    auto formatted = static_cast<Unicode::ListFormat const*>(list_format)->format(string_list(list, list_size));
    write_utf16_to(output, formatted.utf16_view());
}

extern "C" void unicode_list_format_format_to_parts(void const* list_format, UnicodeIntlText const* list, size_t list_size, void* context, UnicodeAppendListFormatPart append_part)
{
    VERIFY(list_format);
    auto parts = static_cast<Unicode::ListFormat const*>(list_format)->format_to_parts(string_list(list, list_size));
    for (auto const& part : parts) {
        with_code_units(part.type.utf16_view(), [&](u16 const* type, size_t type_length) {
            with_code_units(part.value.utf16_view(), [&](u16 const* value, size_t value_length) {
                append_part(context, type, type_length, value, value_length);
            });
        });
    }
}

extern "C" void unicode_list_format_destroy(void* list_format)
{
    VERIFY(list_format);
    delete static_cast<Unicode::ListFormat*>(list_format);
}

static bool write_display_name_to(UnicodeTextMappingOutput output, Optional<Utf16String> const& display_name)
{
    if (!display_name.has_value())
        return false;
    write_utf16_to(output, display_name->utf16_view());
    return true;
}

extern "C" bool unicode_language_display_name(UnicodeIntlText locale, UnicodeIntlText language, u8 language_display, UnicodeTextMappingOutput output)
{
    VERIFY(language_display <= to_underlying(Unicode::LanguageDisplay::Dialect));
    auto display_name = Unicode::language_display_name(ascii_byte_string(locale), ascii_byte_string(language), static_cast<Unicode::LanguageDisplay>(language_display));
    return write_display_name_to(output, display_name);
}

extern "C" bool unicode_region_display_name(UnicodeIntlText locale, UnicodeIntlText region, UnicodeTextMappingOutput output)
{
    return write_display_name_to(output, Unicode::region_display_name(ascii_byte_string(locale), ascii_byte_string(region)));
}

extern "C" bool unicode_script_display_name(UnicodeIntlText locale, UnicodeIntlText script, UnicodeTextMappingOutput output)
{
    return write_display_name_to(output, Unicode::script_display_name(ascii_byte_string(locale), ascii_byte_string(script)));
}

extern "C" bool unicode_currency_display_name(UnicodeIntlText locale, UnicodeIntlText currency, u8 style, UnicodeTextMappingOutput output)
{
    VERIFY(style <= to_underlying(Unicode::Style::Narrow));
    return write_display_name_to(output, Unicode::currency_display_name(ascii_byte_string(locale), ascii_byte_string(currency), static_cast<Unicode::Style>(style)));
}

extern "C" bool unicode_calendar_display_name(UnicodeIntlText locale, UnicodeIntlText calendar, UnicodeTextMappingOutput output)
{
    return write_display_name_to(output, Unicode::calendar_display_name(ascii_byte_string(locale), ascii_byte_string(calendar)));
}

extern "C" bool unicode_date_time_field_display_name(UnicodeIntlText locale, UnicodeIntlText field, u8 style, UnicodeTextMappingOutput output)
{
    VERIFY(style <= to_underlying(Unicode::Style::Narrow));
    return write_display_name_to(output, Unicode::date_time_field_display_name(ascii_byte_string(locale), ascii_byte_string(field), static_cast<Unicode::Style>(style)));
}

extern "C" void* unicode_segmenter_create(UnicodeIntlText locale, u8 granularity)
{
    VERIFY(granularity <= to_underlying(Unicode::SegmenterGranularity::Word));
    auto segmenter = Unicode::Segmenter::create(text_view(locale), static_cast<Unicode::SegmenterGranularity>(granularity));
    return segmenter.leak_ptr();
}

extern "C" void* unicode_segmenter_clone(void const* segmenter)
{
    VERIFY(segmenter);
    return static_cast<Unicode::Segmenter const*>(segmenter)->clone().leak_ptr();
}

extern "C" void unicode_segmenter_set_segmented_text(void* segmenter, UnicodeIntlText text)
{
    VERIFY(segmenter);
    static_cast<Unicode::Segmenter*>(segmenter)->set_segmented_text(text_view(text));
}

extern "C" size_t unicode_segmenter_current_boundary(void* segmenter)
{
    VERIFY(segmenter);
    return static_cast<Unicode::Segmenter*>(segmenter)->current_boundary();
}

static bool write_boundary_to(size_t* boundary, Optional<size_t> found_boundary)
{
    if (!found_boundary.has_value())
        return false;
    *boundary = *found_boundary;
    return true;
}

extern "C" bool unicode_segmenter_previous_boundary(void* segmenter, size_t index, bool inclusive, size_t* boundary)
{
    VERIFY(segmenter);
    auto found_boundary = static_cast<Unicode::Segmenter*>(segmenter)->previous_boundary(index, inclusive ? Unicode::Segmenter::Inclusive::Yes : Unicode::Segmenter::Inclusive::No);
    return write_boundary_to(boundary, found_boundary);
}

extern "C" bool unicode_segmenter_next_boundary(void* segmenter, size_t index, bool inclusive, size_t* boundary)
{
    VERIFY(segmenter);
    auto found_boundary = static_cast<Unicode::Segmenter*>(segmenter)->next_boundary(index, inclusive ? Unicode::Segmenter::Inclusive::Yes : Unicode::Segmenter::Inclusive::No);
    return write_boundary_to(boundary, found_boundary);
}

extern "C" bool unicode_segmenter_is_current_boundary_word_like(void const* segmenter)
{
    VERIFY(segmenter);
    return static_cast<Unicode::Segmenter const*>(segmenter)->is_current_boundary_word_like();
}

extern "C" void unicode_segmenter_destroy(void* segmenter)
{
    VERIFY(segmenter);
    delete static_cast<Unicode::Segmenter*>(segmenter);
}
