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

class Wtf8FlyString {
    AK_MAKE_DEFAULT_MOVABLE(Wtf8FlyString);
    AK_MAKE_DEFAULT_COPYABLE(Wtf8FlyString);

public:
    Wtf8FlyString() = default;

    static ErrorOr<Wtf8FlyString> from_wtf8(StringView);
    static Wtf8FlyString from_wtf8_without_validation(ReadonlyBytes);
    template<typename T>
    requires(IsOneOf<RemoveCVReference<T>, ByteString, DeprecatedFlyString, Wtf8FlyString, String>)
    static ErrorOr<String> from_wtf8(T&&) = delete;

    Wtf8FlyString(String const&);
    Wtf8FlyString& operator=(String const&);

    [[nodiscard]] bool is_empty() const;
    [[nodiscard]] unsigned hash() const;
    [[nodiscard]] u32 ascii_case_insensitive_hash() const;

    explicit operator String() const;
    String to_string() const;

    [[nodiscard]] Wtf8ByteView code_points() const;
    [[nodiscard]] ReadonlyBytes bytes() const;
    [[nodiscard]] StringView bytes_as_string_view() const;

    [[nodiscard]] ALWAYS_INLINE bool operator==(Wtf8FlyString const& other) const { return m_data.raw({}) == other.m_data.raw({}); }
    [[nodiscard]] bool operator==(String const&) const;
    [[nodiscard]] bool operator==(StringView) const;
    [[nodiscard]] bool operator==(char const*) const;

    [[nodiscard]] int operator<=>(Wtf8FlyString const& other) const;

    static void did_destroy_fly_string_data(Badge<Detail::StringData>, Detail::StringData const&);
    [[nodiscard]] Detail::StringBase data(Badge<String>) const;

    // This is primarily interesting to unit tests.
    [[nodiscard]] static size_t number_of_fly_strings();

    // FIXME: Remove these once all code has been ported to Wtf8FlyString
    [[nodiscard]] DeprecatedFlyString to_deprecated_fly_string() const;
    static ErrorOr<Wtf8FlyString> from_deprecated_fly_string(DeprecatedFlyString const&);
    template<typename T>
    requires(IsSame<RemoveCVReference<T>, StringView>)
    static ErrorOr<String> from_deprecated_fly_string(T&&) = delete;

    // Compare this Wtf8FlyString against another string with ASCII caseless matching.
    [[nodiscard]] bool equals_ignoring_ascii_case(Wtf8FlyString const&) const;
    [[nodiscard]] bool equals_ignoring_ascii_case(StringView) const;

    [[nodiscard]] Wtf8FlyString to_ascii_lowercase() const;
    [[nodiscard]] Wtf8FlyString to_ascii_uppercase() const;

    [[nodiscard]] bool starts_with_bytes(StringView, CaseSensitivity = CaseSensitivity::CaseSensitive) const;

    [[nodiscard]] bool ends_with_bytes(StringView, CaseSensitivity = CaseSensitivity::CaseSensitive) const;

    template<typename... Ts>
    [[nodiscard]] ALWAYS_INLINE constexpr bool is_one_of(Ts&&... strings) const
    {
        return (... || this->operator==(forward<Ts>(strings)));
    }

private:
    friend class Optional<Wtf8FlyString>;

    explicit Wtf8FlyString(nullptr_t)
        : m_data(Detail::StringBase(nullptr))
    {
    }

    explicit Wtf8FlyString(Detail::StringBase data)
        : m_data(move(data))
    {
    }

    Detail::StringBase m_data;

    bool is_invalid() const { return m_data.is_invalid(); }
};

template<>
class Optional<Wtf8FlyString> : public OptionalBase<Wtf8FlyString> {
    template<typename U>
    friend class Optional;

public:
    using ValueType = Wtf8FlyString;

    Optional() = default;

    template<SameAs<OptionalNone> V>
    Optional(V) { }

    Optional(Optional<Wtf8FlyString> const& other)
    {
        if (other.has_value())
            m_value = other.m_value;
    }

    Optional(Optional&& other)
        : m_value(other.m_value)
    {
    }

    template<typename U = Wtf8FlyString>
    requires(!IsSame<OptionalNone, RemoveCVReference<U>>)
    explicit(!IsConvertible<U&&, Wtf8FlyString>) Optional(U&& value)
    requires(!IsSame<RemoveCVReference<U>, Optional<Wtf8FlyString>> && IsConstructible<Wtf8FlyString, U &&>)
        : m_value(forward<U>(value))
    {
    }

    template<SameAs<OptionalNone> V>
    Optional& operator=(V)
    {
        clear();
        return *this;
    }

    Optional& operator=(Optional const& other)
    {
        if (this != &other) {
            clear();
            m_value = other.m_value;
        }
        return *this;
    }

    Optional& operator=(Optional&& other)
    {
        if (this != &other) {
            clear();
            m_value = other.m_value;
        }
        return *this;
    }

    template<typename O>
    ALWAYS_INLINE bool operator==(Optional<O> const& other) const
    {
        return has_value() == other.has_value() && (!has_value() || value() == other.value());
    }

    template<typename O>
    ALWAYS_INLINE bool operator==(O const& other) const
    {
        return has_value() && value() == other;
    }

    void clear()
    {
        m_value = Wtf8FlyString(nullptr);
    }

    [[nodiscard]] bool has_value() const
    {
        return !m_value.is_invalid();
    }

    [[nodiscard]] Wtf8FlyString& value() &
    {
        VERIFY(has_value());
        return m_value;
    }

    [[nodiscard]] Wtf8FlyString const& value() const&
    {
        VERIFY(has_value());
        return m_value;
    }

    [[nodiscard]] Wtf8FlyString value() &&
    {
        return release_value();
    }

    [[nodiscard]] Wtf8FlyString release_value()
    {
        VERIFY(has_value());
        Wtf8FlyString released_value = m_value;
        clear();
        return released_value;
    }

private:
    Wtf8FlyString m_value = Wtf8FlyString(nullptr);
};

template<>
struct Traits<Wtf8FlyString> : public DefaultTraits<Wtf8FlyString> {
    static unsigned hash(Wtf8FlyString const&);
};

template<>
struct Formatter<Wtf8FlyString> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder&, Wtf8FlyString const&);
};

struct ASCIICaseInsensitiveWtf8FlyStringTraits : public Traits<String> {
    static unsigned hash(Wtf8FlyString const& s) { return s.ascii_case_insensitive_hash(); }
    static bool equals(Wtf8FlyString const& a, Wtf8FlyString const& b) { return a.equals_ignoring_ascii_case(b); }
};

}

[[nodiscard]] ALWAYS_INLINE AK::Wtf8FlyString operator""_fly_string(char const* cstring, size_t length)
{
    return AK::Wtf8FlyString::from_wtf8(AK::StringView(cstring, length)).release_value();
}

#if USING_AK_GLOBALLY
using AK::Wtf8FlyString;
#endif
