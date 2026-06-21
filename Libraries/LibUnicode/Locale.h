/*
 * Copyright (c) 2021-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/CharacterTypes.h>
#include <AK/IterationDecision.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibUnicode/Forward.h>

namespace Unicode {

struct LanguageID {
    String to_string() const;
    Utf16String to_utf16_string() const;
    bool operator==(LanguageID const&) const = default;

    bool is_root { false };
    Optional<Utf16String> language {};
    Optional<Utf16String> script {};
    Optional<Utf16String> region {};
    Vector<Utf16String> variants {};
};

struct Keyword {
    Utf16String key {};
    Utf16String value {};
};

struct LocaleExtension {
    Vector<Utf16String> attributes {};
    Vector<Keyword> keywords {};
};

struct TransformedField {
    Utf16String key {};
    Utf16String value {};
};

struct TransformedExtension {
    Optional<LanguageID> language {};
    Vector<TransformedField> fields {};
};

struct OtherExtension {
    char key {};
    Utf16String value {};
};

using Extension = AK::Variant<LocaleExtension, TransformedExtension, OtherExtension>;

struct LocaleID {
    String to_string() const;
    Utf16String to_utf16_string() const;

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

    template<typename ExtensionType, typename Callback>
    void for_each_extension_of_type(Callback&& callback)
    {
        for (auto& extension : extensions) {
            if (auto* extension_type = extension.get_pointer<ExtensionType>()) {
                if (callback(*extension_type) == IterationDecision::Break)
                    break;
            }
        }
    }

    LanguageID language_id {};
    Vector<Extension> extensions {};
    Vector<Utf16String> private_use_extensions {};
};

enum class Style {
    Long,
    Short,
    Narrow,
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

constexpr bool is_unicode_language_subtag(Utf16View subtag)
{
    // unicode_language_subtag = alpha{2,3} | alpha{5,8}
    if ((subtag.length_in_code_units() < 2) || (subtag.length_in_code_units() == 4) || (subtag.length_in_code_units() > 8))
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

constexpr bool is_unicode_script_subtag(Utf16View subtag)
{
    // unicode_script_subtag = alpha{4}
    if (subtag.length_in_code_units() != 4)
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

constexpr bool is_unicode_region_subtag(Utf16View subtag)
{
    // unicode_region_subtag = (alpha{2} | digit{3})
    if (subtag.length_in_code_units() == 2)
        return all_of(subtag, is_ascii_alpha);
    if (subtag.length_in_code_units() == 3)
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

constexpr bool is_unicode_variant_subtag(Utf16View subtag)
{
    // unicode_variant_subtag = (alphanum{5,8} | digit alphanum{3})
    if ((subtag.length_in_code_units() >= 5) && (subtag.length_in_code_units() <= 8))
        return all_of(subtag, is_ascii_alphanumeric);
    if (subtag.length_in_code_units() == 4)
        return is_ascii_digit(subtag.code_unit_at(0)) && all_of(subtag.substring_view(1), is_ascii_alphanumeric);
    return false;
}

bool is_type_identifier(StringView);
bool is_type_identifier(Utf16View);

Optional<LanguageID> parse_unicode_language_id(StringView);
Optional<LanguageID> parse_unicode_language_id(Utf16View);
Optional<LocaleID> parse_unicode_locale_id(StringView);
Optional<LocaleID> parse_unicode_locale_id(Utf16View);

Utf16String canonicalize_unicode_locale_id(StringView);
Utf16String canonicalize_unicode_locale_id(Utf16View);
Utf16String canonicalize_unicode_extension_values(StringView key, Utf16View value);

Utf16View default_locale();
bool is_locale_available(StringView locale);

Style style_from_string(StringView style);
Style style_from_string(Utf16View style);
Utf16String style_to_string(Style style);

Optional<Utf16String> add_likely_subtags(Utf16View);
Optional<Utf16String> remove_likely_subtags(Utf16View);

bool is_locale_character_ordering_right_to_left(Utf16View locale);

}
