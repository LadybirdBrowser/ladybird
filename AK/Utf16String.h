/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Error.h>
#include <AK/Format.h>
#include <AK/NonnullRefPtr.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Traits.h>
#include <AK/UnicodeUtils.h>
#include <AK/Utf16StringBase.h>
#include <AK/Utf16StringData.h>
#include <AK/Utf16View.h>
#include <AK/Utf8View.h>

namespace AK {

// Utf16String is a strongly owned sequence of Unicode code points encoded as UTF-16.
//
// The data may or may not be heap-allocated, and may or may not be reference counted. As a memory optimization, if the
// UTF-16 string is entirely ASCII, the string is stored as 8-bit bytes.
class [[nodiscard]] Utf16String : public Detail::Utf16StringBase {
public:
    using Utf16StringBase::Utf16StringBase;

    explicit constexpr Utf16String(Utf16StringBase&& base)
        : Utf16StringBase(move(base))
    {
    }

    ALWAYS_INLINE static Utf16String from_utf8(StringView utf8_string)
    {
        VERIFY(Utf8View { utf8_string }.validate());
        return from_utf8_without_validation(utf8_string);
    }

    ALWAYS_INLINE static Utf16String from_utf8(String const& utf8_string)
    {
        return from_utf8_without_validation(utf8_string);
    }

    enum class WithBOMHandling {
        No,
        Yes,
    };
    static Utf16String from_utf8_with_replacement_character(StringView, WithBOMHandling = WithBOMHandling::Yes);

    ALWAYS_INLINE static ErrorOr<Utf16String> try_from_utf8(StringView utf8_string)
    {
        if (!Utf8View { utf8_string }.validate())
            return Error::from_string_literal("Input was not valid UTF-8");
        return from_utf8_without_validation(utf8_string);
    }

    ALWAYS_INLINE static Utf16String from_utf16(Utf16View const& utf16_string)
    {
        VERIFY(utf16_string.validate());
        return from_utf16_without_validation(utf16_string);
    }

    ALWAYS_INLINE static ErrorOr<Utf16String> try_from_utf16(Utf16View const& utf16_string)
    {
        if (!utf16_string.validate())
            return Error::from_string_literal("Input was not valid UTF-16");
        return from_utf16_without_validation(utf16_string);
    }

    static Utf16String from_utf8_without_validation(StringView);
    static Utf16String from_utf16_without_validation(Utf16View const&);
    static Utf16String from_utf32(Utf32View const&);

    template<typename T>
    requires(IsOneOf<RemoveCVReference<T>, Utf16String, Utf16FlyString>)
    static Utf16String from_utf16(T&&) = delete;

    template<typename T>
    requires(IsOneOf<RemoveCVReference<T>, Utf16String, Utf16FlyString>)
    static ErrorOr<Utf16String> try_from_utf16(T&&) = delete;

    template<typename T>
    requires(IsOneOf<RemoveCVReference<T>, Utf16String, Utf16FlyString>)
    static Utf16String from_utf16_without_validation(T&&) = delete;

    ALWAYS_INLINE static Utf16String from_code_point(u32 code_point)
    {
        Array<char16_t, 2> code_units;
        size_t length_in_code_units = 0;

        (void)UnicodeUtils::code_point_to_utf16(code_point, [&](auto code_unit) {
            code_units[length_in_code_units++] = code_unit;
        });

        return from_utf16_without_validation({ code_units.data(), length_in_code_units });
    }

    template<typename... Parameters>
    ALWAYS_INLINE static Utf16String formatted(CheckedFormatString<Parameters...>&& format, Parameters const&... parameters)
    {
        StringBuilder builder(StringBuilder::Mode::UTF16);

        VariadicFormatParams<AllowDebugOnlyFormatters::No, Parameters...> variadic_format_parameters { parameters... };
        MUST(vformat(builder, format.view(), variadic_format_parameters));

        return builder.to_utf16_string();
    }

    template<Arithmetic T>
    ALWAYS_INLINE static Utf16String number(T value)
    {
        return formatted("{}", value);
    }

    template<class SeparatorType, class CollectionType>
    ALWAYS_INLINE static Utf16String join(SeparatorType const& separator, CollectionType const& collection, StringView format = "{}"sv)
    {
        StringBuilder builder(StringBuilder::Mode::UTF16);
        builder.join(separator, collection, format);

        return builder.to_utf16_string();
    }

    static Utf16String repeated(u32 code_point, size_t count);

    ALWAYS_INLINE static Utf16String from_string_builder(Badge<StringBuilder>, StringBuilder& builder)
    {
        VERIFY(builder.utf16_string_view().validate());
        return from_string_builder_without_validation(builder);
    }

    ALWAYS_INLINE static Utf16String from_string_builder_without_validation(Badge<StringBuilder>, StringBuilder& builder)
    {
        return from_string_builder_without_validation(builder);
    }

    ALWAYS_INLINE Utf16String to_ascii_lowercase() const
    {
        auto view = utf16_view();

        if (view.has_ascii_storage()) {
            if (!any_of(view.ascii_span(), is_ascii_upper_alpha))
                return *this;
        } else {
            if (!any_of(view.utf16_span(), is_ascii_upper_alpha))
                return *this;
        }

        return view.to_ascii_lowercase();
    }

    ALWAYS_INLINE Utf16String to_ascii_uppercase() const
    {
        auto view = utf16_view();

        if (view.has_ascii_storage()) {
            if (!any_of(view.ascii_span(), is_ascii_lower_alpha))
                return *this;
        } else {
            if (!any_of(view.utf16_span(), is_ascii_lower_alpha))
                return *this;
        }

        return view.to_ascii_uppercase();
    }

    ALWAYS_INLINE Utf16String to_ascii_titlecase() const
    {
        return utf16_view().to_ascii_titlecase();
    }

    ALWAYS_INLINE Utf16String replace(Utf16View const& needle, Utf16View const& replacement, ReplaceMode replace_mode) const
    {
        auto view = utf16_view();
        if (view.is_empty() || !view.contains(needle))
            return *this;

        return view.replace(needle, replacement, replace_mode);
    }

    ALWAYS_INLINE Utf16String escape_html_entities() const { return utf16_view().escape_html_entities(); }

private:
    ALWAYS_INLINE explicit Utf16String(NonnullRefPtr<Detail::Utf16StringData const> value)
        : Utf16StringBase(move(value))
    {
    }

    static Utf16String from_string_builder_without_validation(StringBuilder&);
};

template<>
struct Formatter<Utf16String> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder&, Utf16String const&);
};

template<>
struct Traits<Utf16String> : public DefaultTraits<Utf16String> {
    static unsigned hash(Utf16String const& s) { return s.hash(); }
};

}

[[nodiscard]] ALWAYS_INLINE AK::Utf16String operator""_utf16(char const* string, size_t length)
{
    AK::StringView view { string, length };

    ASSERT(AK::Utf8View { view }.validate());
    return AK::Utf16String::from_utf8_without_validation(view);
}

[[nodiscard]] ALWAYS_INLINE AK::Utf16String operator""_utf16(char16_t const* string, size_t length)
{
    AK::Utf16View view { string, length };

    ASSERT(view.validate());
    return AK::Utf16String::from_utf16_without_validation(view);
}
