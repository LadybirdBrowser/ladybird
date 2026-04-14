/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/Optional.h>
#include <AK/Traits.h>
#include <AK/Utf16String.h>

namespace AK {

class [[nodiscard]] Utf16FlyString {
    AK_MAKE_DEFAULT_MOVABLE(Utf16FlyString);
    AK_MAKE_DEFAULT_COPYABLE(Utf16FlyString);

public:
    constexpr Utf16FlyString() = default;

    static Utf16FlyString from_utf8(StringView);
    static Utf16FlyString from_utf8(String const& string) { return from_utf8_without_validation(string); }
    static Utf16FlyString from_utf8(FlyString const& string) { return from_utf8_without_validation(string); }
    static Utf16FlyString from_utf8_without_validation(StringView);
    static Utf16FlyString from_utf8_but_should_be_ported_to_utf16(StringView string) { return from_utf8_without_validation(string); }

    static Utf16FlyString from_utf16(Utf16View const&);

    template<typename T>
    requires(IsOneOf<RemoveCVReference<T>, Utf16String, Utf16FlyString>)
    static Utf16FlyString from_utf16(T&&) = delete;

    Utf16FlyString(Utf16String const&);

    [[nodiscard]] ALWAYS_INLINE Utf16View view() const LIFETIME_BOUND { return m_data.utf16_view(); }

    ALWAYS_INLINE explicit operator Utf16String() const { return to_utf16_string(); }

    ALWAYS_INLINE operator Utf16View() const& LIFETIME_BOUND { return view(); }
    explicit operator Utf16View() const&& = delete;

    ALWAYS_INLINE Utf16String to_utf16_string() const
    {
        Detail::Utf16StringBase copy { m_data };
        return Utf16String { move(copy) };
    }

    ALWAYS_INLINE Utf16FlyString to_ascii_lowercase() const
    {
        auto view = m_data.utf16_view();

        if (view.has_ascii_storage()) {
            if (!any_of(view.ascii_span(), is_ascii_upper_alpha))
                return *this;
        } else {
            if (!any_of(view.utf16_span(), is_ascii_upper_alpha))
                return *this;
        }

        return view.to_ascii_lowercase();
    }

    ALWAYS_INLINE Utf16FlyString to_ascii_uppercase() const
    {
        auto view = m_data.utf16_view();

        if (view.has_ascii_storage()) {
            if (!any_of(view.ascii_span(), is_ascii_lower_alpha))
                return *this;
        } else {
            if (!any_of(view.utf16_span(), is_ascii_lower_alpha))
                return *this;
        }

        return view.to_ascii_uppercase();
    }

    ALWAYS_INLINE Utf16FlyString to_ascii_titlecase() const
    {
        return view().to_ascii_titlecase();
    }

    template<Arithmetic T>
    ALWAYS_INLINE Optional<T> to_number(TrimWhitespace trim_whitespace = TrimWhitespace::Yes, int base = 10) const
    {
        return m_data.to_number<T>(trim_whitespace, base);
    }

    ALWAYS_INLINE Utf16FlyString& operator=(Utf16String const& string)
    {
        *this = Utf16FlyString { string };
        return *this;
    }

    [[nodiscard]] ALWAYS_INLINE bool operator==(Utf16FlyString const& other) const { return m_data.raw({}) == other.m_data.raw({}); }
    [[nodiscard]] ALWAYS_INLINE bool operator==(Utf16String const& other) const { return m_data == other; }
    [[nodiscard]] ALWAYS_INLINE bool operator==(Utf16View const& other) const { return m_data == other; }
    [[nodiscard]] ALWAYS_INLINE bool operator==(StringView other) const { return m_data == other; }

    [[nodiscard]] ALWAYS_INLINE int operator<=>(Utf16FlyString const& other) const { return m_data.operator<=>(other.m_data); }
    [[nodiscard]] ALWAYS_INLINE int operator<=>(Utf16View const& other) const { return m_data.operator<=>(other); }

    [[nodiscard]] ALWAYS_INLINE bool equals_ignoring_ascii_case(Utf16FlyString const& other) const
    {
        if (*this == other)
            return true;
        return m_data.equals_ignoring_ascii_case(other.m_data);
    }

    [[nodiscard]] ALWAYS_INLINE bool equals_ignoring_ascii_case(Utf16View const& other) const { return m_data.equals_ignoring_ascii_case(other); }

    template<typename... Ts>
    [[nodiscard]] ALWAYS_INLINE bool is_one_of(Ts&&... strings) const
    {
        return (this->operator==(forward<Ts>(strings)) || ...);
    }

    template<typename... Ts>
    [[nodiscard]] ALWAYS_INLINE bool is_one_of_ignoring_ascii_case(Ts&&... strings) const
    {
        return (this->equals_ignoring_ascii_case(forward<Ts>(strings)) || ...);
    }

    [[nodiscard]] ALWAYS_INLINE u32 hash() const { return m_data.hash(); }
    [[nodiscard]] ALWAYS_INLINE bool is_empty() const { return m_data.is_empty(); }
    [[nodiscard]] ALWAYS_INLINE bool is_ascii() const { return m_data.is_ascii(); }

    [[nodiscard]] ALWAYS_INLINE size_t length_in_code_units() const { return m_data.length_in_code_units(); }
    [[nodiscard]] ALWAYS_INLINE size_t length_in_code_points() const { return m_data.length_in_code_points(); }

    [[nodiscard]] ALWAYS_INLINE char16_t code_unit_at(size_t code_unit_offset) const { return m_data.code_unit_at(code_unit_offset); }
    [[nodiscard]] ALWAYS_INLINE u32 code_point_at(size_t code_unit_offset) const { return m_data.code_point_at(code_unit_offset); }

    [[nodiscard]] ALWAYS_INLINE size_t code_unit_offset_of(size_t code_point_offset) const { return m_data.code_unit_offset_of(code_point_offset); }
    [[nodiscard]] ALWAYS_INLINE size_t code_point_offset_of(size_t code_unit_offset) const { return m_data.code_point_offset_of(code_unit_offset); }

    constexpr Utf16FlyString(Badge<SentinelOptionalTraits<Utf16FlyString>>, nullptr_t)
        : m_data(Badge<Utf16FlyString> {}, nullptr)
    {
    }

    [[nodiscard]] constexpr bool is_invalid(Badge<SentinelOptionalTraits<Utf16FlyString>>) const { return m_data.raw({}) == 0; }

    // This is primarily interesting to unit tests.
    [[nodiscard]] static size_t number_of_utf16_fly_strings();

    [[nodiscard]] static constexpr Utf16FlyString from_ascii_short_string_without_validation(char const* data, size_t length)
    {
        VERIFY(length <= Detail::MAX_SHORT_STRING_BYTE_COUNT);
        auto short_string = Detail::ShortString::create_with_byte_count(length);
        for (size_t i = 0; i < length; ++i)
            short_string.storage[i] = static_cast<u8>(data[i]);
        return Utf16FlyString { Detail::Utf16StringBase { short_string } };
    }

    [[nodiscard]] static constexpr Utf16FlyString from_ascii_short_string_without_validation(char16_t const* data, size_t length)
    {
        VERIFY(length <= Detail::MAX_SHORT_STRING_BYTE_COUNT);
        auto short_string = Detail::ShortString::create_with_byte_count(length);
        for (size_t i = 0; i < length; ++i)
            short_string.storage[i] = static_cast<u8>(data[i]);
        return Utf16FlyString { Detail::Utf16StringBase { short_string } };
    }

private:
    ALWAYS_INLINE constexpr explicit Utf16FlyString(Detail::Utf16StringBase data)
        : m_data(move(data))
    {
    }

    template<typename ViewType>
    static Optional<Utf16FlyString> create_fly_string_from_cache(ViewType const&);

    Detail::Utf16StringBase m_data;
};

template<>
struct SentinelOptionalTraits<Utf16FlyString> {
    static constexpr Utf16FlyString sentinel_value() { return Utf16FlyString({}, nullptr); }
    static constexpr bool is_sentinel(Utf16FlyString const& value) { return value.is_invalid({}); }
};

template<>
class Optional<Utf16FlyString> : public SentinelOptional<Utf16FlyString> {
public:
    using SentinelOptional::SentinelOptional;
};

template<>
struct Traits<Utf16FlyString> : public DefaultTraits<Utf16FlyString> {
    static unsigned hash(Utf16FlyString const& string) { return string.hash(); }
    static constexpr bool may_have_slow_equality_check() { return false; }
};

template<>
struct Formatter<Utf16FlyString> : Formatter<Utf16String> {
    ErrorOr<void> format(FormatBuilder& builder, Utf16FlyString const& string)
    {
        return Formatter<Utf16String>::format(builder, string.to_utf16_string());
    }
};

namespace Detail {

template<>
inline constexpr bool IsHashCompatible<Utf16View, Utf16FlyString> = true;

template<>
inline constexpr bool IsHashCompatible<Utf16FlyString, Utf16View> = true;

template<>
inline constexpr bool IsHashCompatible<Utf16String, Utf16FlyString> = true;

template<>
inline constexpr bool IsHashCompatible<Utf16FlyString, Utf16String> = true;

}

}

[[nodiscard]] ALWAYS_INLINE constexpr AK::Utf16FlyString operator""_utf16_fly_string(char const* string, size_t length)
{
    // OPTIMIZATION: Short ASCII strings become compile-time constants with no runtime validation or table lookup.
    if (length <= AK::Detail::MAX_SHORT_STRING_BYTE_COUNT
        && AK::all_of(string, string + length, AK::is_ascii)) {
        return AK::Utf16FlyString::from_ascii_short_string_without_validation(string, length);
    }

    AK::StringView view { string, length };

    ASSERT(AK::Utf8View { view }.validate());
    return AK::Utf16FlyString::from_utf8_without_validation(view);
}

[[nodiscard]] ALWAYS_INLINE constexpr AK::Utf16FlyString operator""_utf16_fly_string(char16_t const* string, size_t length)
{
    // OPTIMIZATION: Short ASCII strings become compile-time constants with no runtime work.
    if (length <= AK::Detail::MAX_SHORT_STRING_BYTE_COUNT
        && AK::all_of(string, string + length, AK::is_ascii))
        return AK::Utf16FlyString::from_ascii_short_string_without_validation(string, length);

    return AK::Utf16FlyString::from_utf16({ string, length });
}
