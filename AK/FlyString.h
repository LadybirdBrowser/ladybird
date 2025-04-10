/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Format.h>
#include <AK/Optional.h>
#include <AK/Platform.h>
#include <AK/String.h>
#include <AK/Traits.h>
#include <AK/Types.h>

namespace AK {

class FlyString {
    AK_MAKE_DEFAULT_MOVABLE(FlyString);
    AK_MAKE_DEFAULT_COPYABLE(FlyString);

public:
    FlyString() = default;

    static ErrorOr<FlyString> from_utf8(StringView);
    static FlyString from_utf8_without_validation(ReadonlyBytes);
    template<typename T>
    requires(IsOneOf<RemoveCVReference<T>, ByteString, FlyString, String>)
    static ErrorOr<String> from_utf8(T&&) = delete;

    FlyString(String const&);
    FlyString& operator=(String const&);

    [[nodiscard]] bool is_empty() const { return m_data.byte_count() == 0; }
    [[nodiscard]] unsigned hash() const { return m_data.hash(); }
    [[nodiscard]] u32 ascii_case_insensitive_hash() const;

    explicit operator String() const;
    String to_string() const;

    [[nodiscard]] Utf8View code_points() const;
    [[nodiscard]] ReadonlyBytes bytes() const { return m_data.bytes(); }
    [[nodiscard]] StringView bytes_as_string_view() const { return m_data.bytes(); }

    [[nodiscard]] ALWAYS_INLINE bool operator==(FlyString const& other) const { return m_data.raw(Badge<FlyString> {}) == other.m_data.raw(Badge<FlyString> {}); }
    [[nodiscard]] bool operator==(String const& other) const { return m_data == other; }
    [[nodiscard]] bool operator==(StringView) const;
    [[nodiscard]] bool operator==(char const*) const;

    [[nodiscard]] int operator<=>(FlyString const& other) const;

    [[nodiscard]] Detail::StringBase data(Badge<String>) const;

    // This is primarily interesting to unit tests.
    [[nodiscard]] static size_t number_of_fly_strings();

    template<typename T>
    requires(IsSame<RemoveCVReference<T>, StringView>)
    static ErrorOr<String> from_deprecated_fly_string(T&&) = delete;

    // Compare this FlyString against another string with ASCII caseless matching.
    [[nodiscard]] bool equals_ignoring_ascii_case(FlyString const&) const;
    [[nodiscard]] bool equals_ignoring_ascii_case(StringView) const;

    [[nodiscard]] FlyString to_ascii_lowercase() const;
    [[nodiscard]] FlyString to_ascii_uppercase() const;

    [[nodiscard]] bool starts_with_bytes(StringView, CaseSensitivity = CaseSensitivity::CaseSensitive) const;

    [[nodiscard]] bool ends_with_bytes(StringView, CaseSensitivity = CaseSensitivity::CaseSensitive) const;

    template<typename... Ts>
    [[nodiscard]] ALWAYS_INLINE constexpr bool is_one_of(Ts&&... strings) const
    {
        return (... || this->operator==(forward<Ts>(strings)));
    }

    template<typename... Ts>
    [[nodiscard]] ALWAYS_INLINE constexpr bool is_one_of_ignoring_ascii_case(Ts&&... strings) const
    {
        return (... || this->equals_ignoring_ascii_case(forward<Ts>(strings)));
    }

private:
    friend class Optional<FlyString>;

    explicit constexpr FlyString(nullptr_t)
        : m_data(nullptr)
    {
    }

    explicit constexpr FlyString(Detail::StringBase data)
        : m_data(move(data))
    {
    }

    Detail::StringBase m_data;

    constexpr bool is_invalid() const { return m_data.raw(Badge<FlyString> {}) == 0; }
};

void did_destroy_fly_string_data(Badge<Detail::StringData>, Detail::StringData const&);

template<>
class Optional<FlyString> : public OptionalBase<FlyString> {
    template<typename U>
    friend class Optional;

public:
    using ValueType = FlyString;

    constexpr Optional() = default;

    template<SameAs<OptionalNone> V>
    constexpr Optional(V) { }

    constexpr Optional(Optional<FlyString> const& other)
    {
        if (other.has_value())
            m_value = other.m_value;
    }

    constexpr Optional(Optional&& other)
        : m_value(move(other.m_value))
    {
    }

    template<typename U = FlyString>
    requires(!IsSame<OptionalNone, RemoveCVReference<U>>)
    explicit(!IsConvertible<U&&, FlyString>) constexpr Optional(U&& value)
    requires(!IsSame<RemoveCVReference<U>, Optional<FlyString>> && IsConstructible<FlyString, U &&>)
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
        if (this != &other) {
            clear();
            m_value = other.m_value;
        }
        return *this;
    }

    constexpr Optional& operator=(Optional&& other)
    {
        if (this != &other) {
            clear();
            m_value = other.m_value;
        }
        return *this;
    }

    constexpr void clear()
    {
        m_value = FlyString(nullptr);
    }

    [[nodiscard]] constexpr bool has_value() const
    {
        return !m_value.is_invalid();
    }

    [[nodiscard]] constexpr FlyString& value() &
    {
        VERIFY(has_value());
        return m_value;
    }

    [[nodiscard]] constexpr FlyString const& value() const&
    {
        VERIFY(has_value());
        return m_value;
    }

    [[nodiscard]] constexpr FlyString value() &&
    {
        return release_value();
    }

    [[nodiscard]] constexpr FlyString release_value()
    {
        VERIFY(has_value());
        FlyString released_value = move(m_value);
        clear();
        return released_value;
    }

private:
    FlyString m_value = FlyString(nullptr);
};

template<>
struct Traits<FlyString> : public DefaultTraits<FlyString> {
    static unsigned hash(FlyString const&);
};

template<>
struct Formatter<FlyString> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder&, FlyString const&);
};

struct ASCIICaseInsensitiveFlyStringTraits : public Traits<String> {
    static unsigned hash(FlyString const& s) { return s.ascii_case_insensitive_hash(); }
    static bool equals(FlyString const& a, FlyString const& b) { return a.equals_ignoring_ascii_case(b); }
};

}

[[nodiscard]] ALWAYS_INLINE AK::FlyString operator""_fly_string(char const* cstring, size_t length)
{
    ASSERT(Utf8View(AK::StringView(cstring, length)).validate());
    return AK::FlyString::from_utf8_without_validation({ cstring, length });
}

#if USING_AK_GLOBALLY
using AK::FlyString;
#endif
