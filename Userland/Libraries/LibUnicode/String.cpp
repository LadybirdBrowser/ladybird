/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <LibUnicode/ICU.h>

#include <unicode/bytestream.h>
#include <unicode/casemap.h>
#include <unicode/stringoptions.h>
#include <unicode/translit.h>

// This file contains definitions of AK::String methods which require UCD data.

namespace AK {

struct ResolvedLocale {
    ByteString buffer;
    char const* locale { nullptr };
};

static ResolvedLocale resolve_locale(Optional<StringView> const& locale)
{
    if (!locale.has_value())
        return {};

    ResolvedLocale resolved_locale;
    resolved_locale.buffer = *locale;
    resolved_locale.locale = resolved_locale.buffer.characters();

    return resolved_locale;
}

ErrorOr<String> String::to_lowercase(Optional<StringView> const& locale) const
{
    UErrorCode status = U_ZERO_ERROR;

    StringBuilder builder { bytes_as_string_view().length() };
    icu::StringByteSink sink { &builder };

    auto resolved_locale = resolve_locale(locale);

    icu::CaseMap::utf8ToLower(resolved_locale.locale, 0, Unicode::icu_string_piece(*this), sink, nullptr, status);
    if (Unicode::icu_failure(status))
        return Error::from_string_literal("Unable to convert string to lowercase");

    return builder.to_string_without_validation();
}

ErrorOr<String> String::to_uppercase(Optional<StringView> const& locale) const
{
    UErrorCode status = U_ZERO_ERROR;

    StringBuilder builder { bytes_as_string_view().length() };
    icu::StringByteSink sink { &builder };

    auto resolved_locale = resolve_locale(locale);

    icu::CaseMap::utf8ToUpper(resolved_locale.locale, 0, Unicode::icu_string_piece(*this), sink, nullptr, status);
    if (Unicode::icu_failure(status))
        return Error::from_string_literal("Unable to convert string to uppercase");

    return builder.to_string_without_validation();
}

ErrorOr<String> String::to_titlecase(Optional<StringView> const& locale, TrailingCodePointTransformation trailing_code_point_transformation) const
{
    UErrorCode status = U_ZERO_ERROR;

    StringBuilder builder { bytes_as_string_view().length() };
    icu::StringByteSink sink { &builder };

    auto resolved_locale = resolve_locale(locale);

    u32 options = 0;
    if (trailing_code_point_transformation == TrailingCodePointTransformation::PreserveExisting)
        options |= U_TITLECASE_NO_LOWERCASE;

    icu::CaseMap::utf8ToTitle(resolved_locale.locale, options, nullptr, Unicode::icu_string_piece(*this), sink, nullptr, status);
    if (Unicode::icu_failure(status))
        return Error::from_string_literal("Unable to convert string to titlecase");

    return builder.to_string_without_validation();
}

ErrorOr<String> String::to_fullwidth() const
{
    UErrorCode status = U_ZERO_ERROR;

    auto const transliterator = adopt_own_if_nonnull(icu::Transliterator::createInstance("Halfwidth-Fullwidth", UTRANS_FORWARD, status));
    if (Unicode::icu_failure(status)) {
        return Error::from_string_literal("Unable to create transliterator");
    }

    auto icu_string = Unicode::icu_string(bytes_as_string_view());
    transliterator->transliterate(icu_string);

    return Unicode::icu_string_to_string(icu_string);
}

static ErrorOr<void> build_casefold_string(StringView string, StringBuilder& builder)
{
    UErrorCode status = U_ZERO_ERROR;

    icu::StringByteSink sink { &builder };

    icu::CaseMap::utf8Fold(0, Unicode::icu_string_piece(string), sink, nullptr, status);
    if (Unicode::icu_failure(status))
        return Error::from_string_literal("Unable to casefold string");

    return {};
}

ErrorOr<String> String::to_casefold() const
{
    StringBuilder builder { bytes_as_string_view().length() };
    TRY(build_casefold_string(*this, builder));

    return builder.to_string_without_validation();
}

bool String::equals_ignoring_case(String const& other) const
{
    StringBuilder lhs_builder { bytes_as_string_view().length() };
    if (build_casefold_string(*this, lhs_builder).is_error())
        return false;

    StringBuilder rhs_builder { other.bytes_as_string_view().length() };
    if (build_casefold_string(other, rhs_builder).is_error())
        return false;

    return lhs_builder.string_view() == rhs_builder.string_view();
}

Optional<size_t> String::find_byte_offset_ignoring_case(StringView needle, size_t from_byte_offset) const
{
    auto haystack = bytes_as_string_view().substring_view(from_byte_offset);
    if (haystack.is_empty())
        return {};

    StringBuilder lhs_builder { haystack.length() };
    if (build_casefold_string(haystack, lhs_builder).is_error())
        return {};

    StringBuilder rhs_builder { needle.length() };
    if (build_casefold_string(needle, rhs_builder).is_error())
        return false;

    if (auto index = lhs_builder.string_view().find(rhs_builder.string_view()); index.has_value())
        return *index + from_byte_offset;

    return {};
}

}
