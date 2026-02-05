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
#include <LibUnicode/Forward.h>

namespace Unicode {

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
String canonicalize_unicode_extension_values(StringView key, StringView value);

StringView default_locale();
bool is_locale_available(StringView locale);

Style style_from_string(StringView style);
StringView style_to_string(Style style);

Optional<String> add_likely_subtags(StringView);
Optional<String> remove_likely_subtags(StringView);

bool is_locale_character_ordering_right_to_left(StringView locale);

}
