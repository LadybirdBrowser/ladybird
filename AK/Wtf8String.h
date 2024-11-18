/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/CharacterTypes.h>
#include <AK/Concepts.h>
#include <AK/Format.h>
#include <AK/Forward.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/Span.h>
#include <AK/StringBase.h>
#include <AK/StringBuilder.h>
#include <AK/StringUtils.h>
#include <AK/StringView.h>
#include <AK/Traits.h>
#include <AK/Types.h>
#include <AK/UnicodeCodePointView.h>
#include <AK/UnicodeUtils.h>
#include <AK/Vector.h>
#include <AK/Wtf8ByteView.h>
#include <AK/Wtf8View.h>

namespace AK {

// FIXME: Remove this when OpenBSD Clang fully supports consteval.
//        And once oss-fuzz updates to clang >15.
//        And once Android ships an NDK with clang >14
#if defined(AK_OS_OPENBSD) || defined(OSS_FUZZ) || defined(AK_OS_ANDROID)
#    define AK_SHORT_STRING_CONSTEVAL constexpr
#else
#    define AK_SHORT_STRING_CONSTEVAL consteval
#endif

// Wtf8String is a strongly owned sequence of Unicode code points encoded as WTF-8.
// The data may or may not be heap-allocated, and may or may not be reference counted.
// There is no guarantee that the underlying bytes are null-terminated.
class Wtf8String : public UnicodeCodePointIterableBase<Wtf8String, Wtf8View, Detail::StringBase> {
public:
    Wtf8View unicode_code_point_view() const&
    {
        return Wtf8View::from_string_view_unchecked(bytes_as_string_view());
    }

    // NOTE: For short strings, we avoid heap allocations by storing them in the data pointer slot.
    static constexpr size_t MAX_SHORT_STRING_BYTE_COUNT = Detail::MAX_SHORT_STRING_BYTE_COUNT;

    using UnicodeCodePointIterableBase::UnicodeCodePointIterableBase;

    // Creates a new Wtf8String from a sequence of WTF-8 encoded code points.
    static ErrorOr<Wtf8String> from_wtf8(StringView);

    enum class WithBOMHandling {
        Yes,
        No,
    };

    // Creates a new Wtf8String using the replacement character for invalid bytes
    [[nodiscard]] static Wtf8String from_wtf8_with_replacement_character(StringView, WithBOMHandling = WithBOMHandling::Yes);

    template<typename T>
    requires(IsOneOf<RemoveCVReference<T>, ByteString, DeprecatedFlyString, Wtf8FlyString, Wtf8String>)
    static ErrorOr<Wtf8String> from_wtf8(T&&) = delete;

    [[nodiscard]] static Wtf8String from_wtf8_without_validation(ReadonlyBytes);

    static ErrorOr<Wtf8String> from_string_builder(Badge<StringBuilder>, StringBuilder&);
    [[nodiscard]] static Wtf8String from_string_builder_without_validation(Badge<StringBuilder>, StringBuilder&);

    // Creates a new Wtf8String from a sequence of UTF-16 encoded code points.
    static ErrorOr<Wtf8String> from_utf16(Wtf16ByteView const&);

    // Creates a new Wtf8String by reading byte_count bytes from a WTF-8 encoded Stream.
    static ErrorOr<Wtf8String> from_stream(Stream&, size_t byte_count);

    // Creates a new Wtf8String from a single code point.
    static constexpr Wtf8String from_code_point(AsciiChar code_point)
    {
        Wtf8String string;
        string.replace_with_new_short_string(1, [&](Bytes buffer) {
            buffer[0] = code_point;
        });

        return string;
    }

    // Creates a new Wtf8String from a single code point.
    static constexpr Wtf8String from_code_point(UnicodeCodePoint code_point)
    {
        Wtf8String string;
        string.replace_with_new_short_string(UnicodeUtils::bytes_to_store_code_point_in_utf8(code_point), [&](Bytes buffer) {
            size_t i = 0;
            (void)UnicodeUtils::code_point_to_utf8(code_point, [&](auto byte) {
                buffer[i++] = static_cast<u8>(byte);
            });
        });

        return string;
    }

    // Creates a new Wtf8String from a single code point.
    static constexpr Wtf8String from_code_point(u32 code_point)
    {
        return from_code_point(UnicodeCodePoint::checked(code_point));
    }

    // Creates a new Wtf8String with a single code point repeated N times.
    static ErrorOr<Wtf8String> repeated(u32 code_point, size_t count);

    // Creates a new Wtf8String from another string, repeated N times.
    static ErrorOr<Wtf8String> repeated(Wtf8String const&, size_t count);

    // Creates a new Wtf8String by case-transforming this Wtf8String. Using these methods require linking LibUnicode into your application.
    ErrorOr<Wtf8String> to_lowercase(Optional<StringView> const& locale = {}) const;
    ErrorOr<Wtf8String> to_uppercase(Optional<StringView> const& locale = {}) const;
    ErrorOr<Wtf8String> to_titlecase(Optional<StringView> const& locale = {}, TrailingCodePointTransformation trailing_code_point_transformation = TrailingCodePointTransformation::Lowercase) const;
    ErrorOr<Wtf8String> to_casefold() const;
    ErrorOr<Wtf8String> to_fullwidth() const;

    [[nodiscard]] Wtf8String to_ascii_lowercase() const;
    [[nodiscard]] Wtf8String to_ascii_uppercase() const;

    // Compare this Wtf8String against another string with caseless matching. Using this method requires linking LibUnicode into your application.
    [[nodiscard]] bool equals_ignoring_case(Wtf8String const&) const;

    [[nodiscard]] bool equals_ignoring_ascii_case(Wtf8String const&) const;
    [[nodiscard]] bool equals_ignoring_ascii_case(StringView) const;

    [[nodiscard]] bool starts_with(u32 code_point) const;
    [[nodiscard]] bool starts_with_bytes(StringView, CaseSensitivity = CaseSensitivity::CaseSensitive) const;

    [[nodiscard]] bool ends_with(u32 code_point) const;
    [[nodiscard]] bool ends_with_bytes(StringView, CaseSensitivity = CaseSensitivity::CaseSensitive) const;

    // Creates a substring with a deep copy of the specified data window.
    ErrorOr<Wtf8String> substring_from_byte_offset(size_t start, size_t byte_count) const;
    ErrorOr<Wtf8String> substring_from_byte_offset(size_t start) const;

    // Creates a substring that strongly references the origin superstring instead of making a deep copy of the data.
    ErrorOr<Wtf8String> substring_from_byte_offset_with_shared_superstring(size_t start, size_t byte_count) const;
    ErrorOr<Wtf8String> substring_from_byte_offset_with_shared_superstring(size_t start) const;

    // Returns an iterable view over the Unicode code points.
    [[nodiscard]] Wtf8ByteView code_points() const&;
    [[nodiscard]] Wtf8ByteView code_points() const&& = delete;

    // Returns true if the Wtf8String is zero-length.
    [[nodiscard]] bool is_empty() const;

    // Returns a StringView covering the full length of the string. Note that iterating this will go byte-at-a-time, not code-point-at-a-time.
    [[nodiscard]] StringView bytes_as_string_view() const&;
    [[nodiscard]] StringView bytes_as_string_view() const&& = delete;

    [[nodiscard]] size_t count(StringView needle) const { return StringUtils::count(bytes_as_string_view(), needle); }

    ErrorOr<Wtf8String> replace(StringView needle, StringView replacement, ReplaceMode replace_mode) const;
    ErrorOr<Wtf8String> reverse() const;

    ErrorOr<Wtf8String> trim(Wtf8ByteView const& code_points_to_trim, TrimMode mode = TrimMode::Both) const;
    ErrorOr<Wtf8String> trim(StringView code_points_to_trim, TrimMode mode = TrimMode::Both) const;
    ErrorOr<Wtf8String> trim_whitespace(TrimMode mode = TrimMode::Both) const;
    ErrorOr<Wtf8String> trim_ascii_whitespace(TrimMode mode = TrimMode::Both) const;

    ErrorOr<Vector<Wtf8String>> split_limit(u32 separator, size_t limit, SplitBehavior = SplitBehavior::Nothing) const;
    ErrorOr<Vector<Wtf8String>> split(u32 separator, SplitBehavior = SplitBehavior::Nothing) const;

    Optional<size_t> find_byte_offset(u32 code_point, size_t from_byte_offset = 0) const;
    Optional<size_t> find_byte_offset(StringView substring, size_t from_byte_offset = 0) const;

    // Using this method requires linking LibUnicode into your application.
    Optional<size_t> find_byte_offset_ignoring_case(StringView, size_t from_byte_offset = 0) const;

    [[nodiscard]] bool operator==(Wtf8String const&) const = default;
    [[nodiscard]] bool operator==(Wtf8FlyString const&) const;
    [[nodiscard]] bool operator==(StringView) const;
    [[nodiscard]] bool operator==(char const* cstring) const;

    // NOTE: WTF-8 is defined in a way that lexicographic ordering of code points is equivalent to lexicographic ordering of bytes.
    [[nodiscard]] int operator<=>(Wtf8String const& other) const { return this->bytes_as_string_view().compare(other.bytes_as_string_view()); }

    template<typename... Ts>
    [[nodiscard]] ALWAYS_INLINE constexpr bool is_one_of(Ts&&... strings) const
    {
        return (this->operator==(forward<Ts>(strings)) || ...);
    }

    [[nodiscard]] bool contains(StringView, CaseSensitivity = CaseSensitivity::CaseSensitive) const;
    [[nodiscard]] bool contains(u32, CaseSensitivity = CaseSensitivity::CaseSensitive) const;

    [[nodiscard]] u32 ascii_case_insensitive_hash() const;

    template<Arithmetic T>
    [[nodiscard]] static Wtf8String number(T value)
    {
        return MUST(formatted("{}", value));
    }

    template<Arithmetic T>
    Optional<T> to_number(TrimWhitespace trim_whitespace = TrimWhitespace::Yes) const
    {
        return bytes_as_string_view().to_number<T>(trim_whitespace);
    }

    static ErrorOr<Wtf8String> vformatted(StringView fmtstr, TypeErasedFormatParams&);

    template<typename... Parameters>
    static ErrorOr<Wtf8String> formatted(CheckedFormatString<Parameters...>&& fmtstr, Parameters const&... parameters)
    {
        VariadicFormatParams<AllowDebugOnlyFormatters::No, Parameters...> variadic_format_parameters { parameters... };
        return vformatted(fmtstr.view(), variadic_format_parameters);
    }

    template<class SeparatorType, class CollectionType>
    static ErrorOr<Wtf8String> join(SeparatorType const& separator, CollectionType const& collection, StringView fmtstr = "{}"sv)
    {
        StringBuilder builder;
        TRY(builder.try_join(separator, collection, fmtstr));
        return builder.to_string();
    }

    // FIXME: Remove these once all code has been ported to Wtf8String
    [[nodiscard]] ByteString to_byte_string() const;
    static ErrorOr<Wtf8String> from_byte_string(ByteString const&);
    template<typename T>
    requires(IsSame<RemoveCVReference<T>, StringView>)
    static ErrorOr<Wtf8String> from_byte_string(T&&) = delete;

private:
    friend class ::AK::Wtf8FlyString;
    friend class Optional<Wtf8String>;

    using ShortString = Detail::ShortString;

    explicit constexpr Wtf8String(StringBase&& base)
        : UnicodeCodePointIterableBase(move(base))
    {
    }

    explicit constexpr Wtf8String(nullptr_t)
        : UnicodeCodePointIterableBase(nullptr)
    {
    }
};

template<>
class Optional<Wtf8String> : public OptionalBase<Wtf8String> {
    template<typename U>
    friend class Optional;

public:
    using ValueType = Wtf8String;

    Optional() = default;

    template<SameAs<OptionalNone> V>
    Optional(V) { }

    Optional(Optional<Wtf8String> const& other)
    {
        if (other.has_value())
            m_value = other.m_value;
    }

    Optional(Optional&& other)
        : m_value(move(other.m_value))
    {
    }

    template<typename U = Wtf8String>
    requires(!IsSame<OptionalNone, RemoveCVReference<U>>)
    explicit(!IsConvertible<U&&, Wtf8String>) Optional(U&& value)
    requires(!IsSame<RemoveCVReference<U>, Optional<Wtf8String>> && IsConstructible<Wtf8String, U &&>)
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
            m_value = other.m_value;
        }
        return *this;
    }

    Optional& operator=(Optional&& other)
    {
        if (this != &other) {
            m_value = move(other.m_value);
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
        m_value = Wtf8String(nullptr);
    }

    [[nodiscard]] bool has_value() const
    {
        return !m_value.is_invalid();
    }

    [[nodiscard]] Wtf8String& value() &
    {
        VERIFY(has_value());
        return m_value;
    }

    [[nodiscard]] Wtf8String const& value() const&
    {
        VERIFY(has_value());
        return m_value;
    }

    [[nodiscard]] Wtf8String value() &&
    {
        return release_value();
    }

    [[nodiscard]] Wtf8String release_value()
    {
        VERIFY(has_value());
        Wtf8String released_value = m_value;
        clear();
        return released_value;
    }

private:
    Wtf8String m_value { nullptr };
};

template<>
struct Traits<Wtf8String> : public DefaultTraits<Wtf8String> {
    static unsigned hash(Wtf8String const&);
};

template<>
struct Formatter<Wtf8String> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder&, Wtf8String const&);
};

struct ASCIICaseInsensitiveStringTraits : public Traits<Wtf8String> {
    static unsigned hash(Wtf8String const& s) { return s.ascii_case_insensitive_hash(); }
    static bool equals(Wtf8String const& a, Wtf8String const& b) { return a.bytes_as_string_view().equals_ignoring_ascii_case(b.bytes_as_string_view()); }
};

}

[[nodiscard]] ALWAYS_INLINE AK::Wtf8String operator""_string(char const* cstring, size_t length)
{
    return AK::Wtf8String::from_wtf8(AK::StringView(cstring, length)).release_value();
}
