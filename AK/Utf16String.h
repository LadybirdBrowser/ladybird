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

    ALWAYS_INLINE static Utf16String from_utf8(FlyString const& utf8_string)
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

    static Utf16String from_utf8_without_validation(StringView);
    static Utf16String from_ascii_without_validation(ReadonlyBytes);

    static Utf16String from_utf16(Utf16View const& utf16_string);

    template<typename T>
    requires(IsOneOf<RemoveCVReference<T>, Utf16String, Utf16FlyString>)
    static Utf16String from_utf16(T&&) = delete;

    static Utf16String from_utf32(Utf32View const&);

    ALWAYS_INLINE static Utf16String from_code_point(u32 code_point)
    {
        Array<char16_t, 2> code_units;
        size_t length_in_code_units = 0;

        (void)UnicodeUtils::code_point_to_utf16(code_point, [&](auto code_unit) {
            code_units[length_in_code_units++] = code_unit;
        });

        return from_utf16({ code_units.data(), length_in_code_units });
    }

    template<typename... Parameters>
    ALWAYS_INLINE static Utf16String formatted(CheckedFormatString<Parameters...>&& format, Parameters const&... parameters)
    {
        StringBuilder builder(StringBuilder::Mode::UTF16);

        VariadicFormatParams<AllowDebugOnlyFormatters::No, Parameters...> variadic_format_parameters { parameters... };
        MUST(vformat(builder, format.view(), variadic_format_parameters));

        return builder.to_utf16_string();
    }

    template<Integral T>
    [[nodiscard]] static Utf16String number(T);

    template<FloatingPoint T>
    [[nodiscard]] static Utf16String number(T value)
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

    Utf16String to_well_formed() const;
    String to_well_formed_utf8() const;

    // These methods require linking LibUnicode.
    Utf16String to_lowercase(Optional<StringView> const& locale = {}) const;
    Utf16String to_uppercase(Optional<StringView> const& locale = {}) const;
    Utf16String to_titlecase(Optional<StringView> const& locale = {}, TrailingCodePointTransformation trailing_code_point_transformation = TrailingCodePointTransformation::Lowercase) const;
    Utf16String to_casefold() const;
    Utf16String to_fullwidth() const;

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

    ALWAYS_INLINE Utf16String replace(char16_t needle, Utf16View const& replacement, ReplaceMode replace_mode) const
    {
        auto view = utf16_view();
        if (view.is_empty() || !view.contains(needle))
            return *this;

        return view.replace(needle, replacement, replace_mode);
    }

    ALWAYS_INLINE Utf16String replace(Utf16View const& needle, Utf16View const& replacement, ReplaceMode replace_mode) const
    {
        auto view = utf16_view();
        if (view.is_empty() || !view.contains(needle))
            return *this;

        return view.replace(needle, replacement, replace_mode);
    }

    ALWAYS_INLINE Utf16String trim(Utf16View const& code_units, TrimMode mode = TrimMode::Both) const
    {
        if (is_empty())
            return {};

        bool needs_trimming = false;

        if (mode == TrimMode::Left || mode == TrimMode::Both)
            needs_trimming |= code_units.contains(code_unit_at(0));
        if (mode == TrimMode::Right || mode == TrimMode::Both)
            needs_trimming |= code_units.contains(code_unit_at(length_in_code_units() - 1));

        if (!needs_trimming)
            return *this;

        return Utf16String::from_utf16(utf16_view().trim(code_units, mode));
    }

    ALWAYS_INLINE Utf16String trim_ascii_whitespace(TrimMode mode = TrimMode::Both) const
    {
        return trim(" \n\t\v\f\r"sv, mode);
    }

    ALWAYS_INLINE Utf16String escape_html_entities() const { return utf16_view().escape_html_entities(); }

    static Utf16String from_string_builder(Badge<StringBuilder>, StringBuilder& builder);
    static ErrorOr<Utf16String> from_ipc_stream(Stream&, size_t length_in_code_units, bool is_ascii);

    constexpr Utf16String(Badge<Optional<Utf16String>>, nullptr_t)
        : Detail::Utf16StringBase(Badge<Utf16String> {}, nullptr)
    {
    }

    [[nodiscard]] constexpr bool is_invalid(Badge<Optional<Utf16String>>) const { return raw() == 0; }

private:
    ALWAYS_INLINE explicit Utf16String(NonnullRefPtr<Detail::Utf16StringData const> value)
        : Utf16StringBase(move(value))
    {
    }
};

template<>
class [[nodiscard]] Optional<Utf16String> : public OptionalBase<Utf16String> {
    template<typename U>
    friend class Optional;

public:
    using ValueType = Utf16String;

    constexpr Optional() = default;

    template<SameAs<OptionalNone> V>
    constexpr Optional(V) { }

    constexpr Optional(Optional<Utf16String> const& other)
        : m_value(other.m_value)
    {
    }

    constexpr Optional(Optional&& other)
        : m_value(move(other.m_value))
    {
    }

    template<typename U = Utf16String>
    requires(!IsSame<OptionalNone, RemoveCVReference<U>>)
    explicit(!IsConvertible<U&&, Utf16String>) constexpr Optional(U&& value)
    requires(!IsSame<RemoveCVReference<U>, Optional<Utf16String>> && IsConstructible<Utf16String, U &&>)
        : m_value(forward<U>(value))
    {
    }

    template<SameAs<OptionalNone> V>
    constexpr Optional& operator=(V)
    {
        clear();
        return *this;
    }

    constexpr Optional& operator=(Optional const& other)
    {
        if (this != &other)
            m_value = other.m_value;
        return *this;
    }

    constexpr Optional& operator=(Optional&& other)
    {
        if (this != &other)
            m_value = move(other.m_value);
        return *this;
    }

    constexpr void clear()
    {
        m_value = empty_value;
    }

    [[nodiscard]] constexpr bool has_value() const
    {
        return !m_value.is_invalid({});
    }

    [[nodiscard]] constexpr Utf16String& value() &
    {
        VERIFY(has_value());
        return m_value;
    }

    [[nodiscard]] constexpr Utf16String const& value() const&
    {
        VERIFY(has_value());
        return m_value;
    }

    [[nodiscard]] constexpr Utf16String value() &&
    {
        return release_value();
    }

    [[nodiscard]] constexpr Utf16String release_value()
    {
        VERIFY(has_value());
        return exchange(m_value, empty_value);
    }

private:
    static constexpr Utf16String empty_value { Badge<Optional<Utf16String>> {}, nullptr };
    Utf16String m_value { empty_value };
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
    return AK::Utf16String::from_utf16({ string, length });
}
