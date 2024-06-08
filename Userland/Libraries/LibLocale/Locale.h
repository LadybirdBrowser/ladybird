/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/CharacterTypes.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibLocale/Forward.h>

namespace Locale {

struct LanguageID {
    String to_string() const;
    bool operator==(LanguageID const&) const = default;

    bool is_root { false };
    Optional<String> language {};
    Optional<String> script {};
    Optional<String> region {};
    Vector<String> variants {};
};

struct Keyword {
    String key {};
    String value {};
};

struct LocaleExtension {
    Vector<String> attributes {};
    Vector<Keyword> keywords {};
};

struct TransformedField {
    String key {};
    String value {};
};

struct TransformedExtension {
    Optional<LanguageID> language {};
    Vector<TransformedField> fields {};
};

struct OtherExtension {
    char key {};
    String value {};
};

using Extension = AK::Variant<LocaleExtension, TransformedExtension, OtherExtension>;

struct LocaleID {
    String to_string() const;

    template<typename ExtensionType>
    Vector<Extension> remove_extension_type()
    {
        Vector<Extension> removed_extensions {};
        auto tmp_extensions = move(extensions);

        for (auto& extension : tmp_extensions) {
            if (extension.has<ExtensionType>())
                removed_extensions.append(move(extension));
            else
                extensions.append(move(extension));
        }

        return removed_extensions;
    }

    LanguageID language_id {};
    Vector<Extension> extensions {};
    Vector<String> private_use_extensions {};
};

enum class Style : u8 {
    Long,
    Short,
    Narrow,
};

struct DisplayPattern {
    StringView locale_pattern;
    StringView locale_separator;
};

struct ListPatterns {
    StringView start;
    StringView middle;
    StringView end;
    StringView pair;
};

// Note: These methods only verify that the provided strings match the EBNF grammar of the
// Unicode identifier subtag (i.e. no validation is done that the tags actually exist).
constexpr bool is_unicode_language_subtag(StringView subtag)
{
    // unicode_language_subtag = alpha{2,3} | alpha{5,8}
    if ((subtag.length() < 2) || (subtag.length() == 4) || (subtag.length() > 8))
        return false;
    return all_of(subtag, is_ascii_alpha);
}

constexpr bool is_unicode_script_subtag(StringView subtag)
{
    // unicode_script_subtag = alpha{4}
    if (subtag.length() != 4)
        return false;
    return all_of(subtag, is_ascii_alpha);
}

constexpr bool is_unicode_region_subtag(StringView subtag)
{
    // unicode_region_subtag = (alpha{2} | digit{3})
    if (subtag.length() == 2)
        return all_of(subtag, is_ascii_alpha);
    if (subtag.length() == 3)
        return all_of(subtag, is_ascii_digit);
    return false;
}

constexpr bool is_unicode_variant_subtag(StringView subtag)
{
    // unicode_variant_subtag = (alphanum{5,8} | digit alphanum{3})
    if ((subtag.length() >= 5) && (subtag.length() <= 8))
        return all_of(subtag, is_ascii_alphanumeric);
    if (subtag.length() == 4)
        return is_ascii_digit(subtag[0]) && all_of(subtag.substring_view(1), is_ascii_alphanumeric);
    return false;
}

bool is_type_identifier(StringView);

Optional<LanguageID> parse_unicode_language_id(StringView);
Optional<LocaleID> parse_unicode_locale_id(StringView);

String canonicalize_unicode_locale_id(StringView);
void canonicalize_unicode_extension_values(StringView key, String& value, bool remove_true);

StringView default_locale();
bool is_locale_available(StringView locale);

ReadonlySpan<StringView> get_available_keyword_values(StringView key);
ReadonlySpan<StringView> get_available_calendars();
ReadonlySpan<StringView> get_available_collation_case_orderings();
ReadonlySpan<StringView> get_available_collation_numeric_orderings();
ReadonlySpan<StringView> get_available_collation_types();
ReadonlySpan<StringView> get_available_hour_cycles();
ReadonlySpan<StringView> get_available_number_systems();

Vector<String> available_currencies();

Style style_from_string(StringView style);
StringView style_to_string(Style style);

Optional<Locale> locale_from_string(StringView locale);
Optional<ListPatternType> list_pattern_type_from_string(StringView list_pattern_type);

Optional<Key> key_from_string(StringView key);
Optional<KeywordCalendar> keyword_ca_from_string(StringView ca);
Optional<KeywordCollation> keyword_co_from_string(StringView co);
Optional<KeywordHours> keyword_hc_from_string(StringView hc);
Optional<KeywordColCaseFirst> keyword_kf_from_string(StringView kf);
Optional<KeywordColNumeric> keyword_kn_from_string(StringView kn);
Optional<KeywordNumbers> keyword_nu_from_string(StringView nu);
Vector<StringView> get_keywords_for_locale(StringView locale, StringView key);
Optional<StringView> get_preferred_keyword_value_for_locale(StringView locale, StringView key);

Optional<ListPatterns> get_locale_list_patterns(StringView locale, StringView type, Style style);

Optional<CharacterOrder> character_order_from_string(StringView character_order);
StringView character_order_to_string(CharacterOrder character_order);
Optional<CharacterOrder> character_order_for_locale(StringView locale);

Optional<LanguageID> add_likely_subtags(LanguageID const& language_id);
Optional<LanguageID> remove_likely_subtags(LanguageID const& language_id);

}
