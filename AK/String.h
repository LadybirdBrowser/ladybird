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
#include <AK/UnicodeUtils.h>
#include <AK/Utf8View.h>
#include <AK/Vector.h>

namespace AK {

// FIXME: Remove this when OpenBSD Clang fully supports consteval.
//        And once oss-fuzz updates to clang >15.
//        And once Android ships an NDK with clang >14
#if defined(AK_OS_OPENBSD) || defined(OSS_FUZZ) || defined(AK_OS_ANDROID)
#    define AK_SHORT_STRING_CONSTEVAL constexpr
#else
#    define AK_SHORT_STRING_CONSTEVAL consteval
#endif

// String is a strongly owned sequence of Unicode code points encoded as UTF-8.
// The data may or may not be heap-allocated, and may or may not be reference counted.
// There is no guarantee that the underlying bytes are null-terminated.
class String : public Detail::StringBase {
    AK_MAKE_DEFAULT_COPYABLE(String);
    AK_MAKE_DEFAULT_MOVABLE(String);

public:
    // NOTE: For short strings, we avoid heap allocations by storing them in the data pointer slot.
    static constexpr size_t MAX_SHORT_STRING_BYTE_COUNT = Detail::MAX_SHORT_STRING_BYTE_COUNT;

    using StringBase::StringBase;

    // Creates a new String from a sequence of UTF-8 encoded code points.
    static ErrorOr<String> from_utf8(StringView);

    enum class WithBOMHandling {
        Yes,
        No,
    };

    // Creates a new String using the replacement character for invalid bytes
    [[nodiscard]] static String from_utf8_with_replacement_character(StringView, WithBOMHandling = WithBOMHandling::Yes);

    template<typename T>
    requires(IsOneOf<RemoveCVReference<T>, ByteString, FlyString, String>)
    static ErrorOr<String> from_utf8(T&&) = delete;

    [[nodiscard]] static String from_utf8_without_validation(ReadonlyBytes);

    static ErrorOr<String> from_string_builder(Badge<StringBuilder>, StringBuilder&);
    [[nodiscard]] static String from_string_builder_without_validation(Badge<StringBuilder>, StringBuilder&);

    // Creates a new String from a sequence of UTF-16 encoded code points.
    static ErrorOr<String> from_utf16(Utf16View const&);
    static ErrorOr<String> from_utf16_le(ReadonlyBytes);
    static ErrorOr<String> from_utf16_be(ReadonlyBytes);

    // Creates a new String by reading byte_count bytes from a UTF-8 encoded Stream.
    static ErrorOr<String> from_stream(Stream&, size_t byte_count);

    // Creates a new String from a single code point.
    static constexpr String from_code_point(u32 code_point)
    {
        VERIFY(is_unicode(code_point));

        String string;
        string.replace_with_new_short_string(UnicodeUtils::bytes_to_store_code_point_in_utf8(code_point), [&](Bytes buffer) {
            size_t i = 0;
            (void)UnicodeUtils::code_point_to_utf8(code_point, [&](auto byte) {
                buffer[i++] = static_cast<u8>(byte);
            });
        });

        return string;
    }

    // Creates a new String with a single code point repeated N times.
    static ErrorOr<String> repeated(u32 code_point, size_t count);

    // Creates a new String from another string, repeated N times.
    static ErrorOr<String> repeated(String const&, size_t count);

    enum class Case {
        Upper,
        Lower,
    };
    [[nodiscard]] static String bijective_base_from(size_t value, Case, unsigned base = 26, StringView map = {});
    [[nodiscard]] static String roman_number_from(size_t value, Case);

    // Creates a new String by case-transforming this String. Using these methods require linking LibUnicode into your application.
    ErrorOr<String> to_lowercase(Optional<StringView> const& locale = {}) const;
    ErrorOr<String> to_uppercase(Optional<StringView> const& locale = {}) const;
    ErrorOr<String> to_titlecase(Optional<StringView> const& locale = {}, TrailingCodePointTransformation trailing_code_point_transformation = TrailingCodePointTransformation::Lowercase) const;
    ErrorOr<String> to_casefold() const;
    ErrorOr<String> to_fullwidth() const;

    [[nodiscard]] String to_ascii_lowercase() const;
    [[nodiscard]] String to_ascii_uppercase() const;
    [[nodiscard]] bool is_ascii() const { return bytes_as_string_view().is_ascii(); }

    // Compare this String against another string with caseless matching. Using this method requires linking LibUnicode into your application.
    [[nodiscard]] bool equals_ignoring_case(String const&) const;

    [[nodiscard]] bool equals_ignoring_ascii_case(String const&) const;
    [[nodiscard]] bool equals_ignoring_ascii_case(StringView) const;

    [[nodiscard]] bool starts_with(u32 code_point) const;
    [[nodiscard]] bool starts_with_bytes(StringView, CaseSensitivity = CaseSensitivity::CaseSensitive) const;

    [[nodiscard]] bool ends_with(u32 code_point) const;
    [[nodiscard]] bool ends_with_bytes(StringView, CaseSensitivity = CaseSensitivity::CaseSensitive) const;

    // Creates a substring with a deep copy of the specified data window.
    ErrorOr<String> substring_from_byte_offset(size_t start, size_t byte_count) const;
    ErrorOr<String> substring_from_byte_offset(size_t start) const;

    // Creates a substring that strongly references the origin superstring instead of making a deep copy of the data.
    ErrorOr<String> substring_from_byte_offset_with_shared_superstring(size_t start, size_t byte_count) const;
    ErrorOr<String> substring_from_byte_offset_with_shared_superstring(size_t start) const;

    // Returns an iterable view over the Unicode code points.
    [[nodiscard]] Utf8View code_points() const&;
    [[nodiscard]] Utf8View code_points() const&& = delete;

    // Returns true if the String is zero-length.
    [[nodiscard]] bool is_empty() const { return byte_count() == 0; }

    // Returns a StringView covering the full length of the string. Note that iterating this will go byte-at-a-time, not code-point-at-a-time.
    [[nodiscard]] StringView bytes_as_string_view() const& { return StringView(bytes()); }
    [[nodiscard]] StringView bytes_as_string_view() const&& = delete;

    [[nodiscard]] size_t count(StringView needle) const { return StringUtils::count(bytes_as_string_view(), needle); }

    ErrorOr<String> replace(StringView needle, StringView replacement, ReplaceMode replace_mode) const;
    ErrorOr<String> reverse() const;

    ErrorOr<String> trim(Utf8View const& code_points_to_trim, TrimMode mode = TrimMode::Both) const;
    ErrorOr<String> trim(StringView code_points_to_trim, TrimMode mode = TrimMode::Both) const;
    ErrorOr<String> trim_whitespace(TrimMode mode = TrimMode::Both) const;
    ErrorOr<String> trim_ascii_whitespace(TrimMode mode = TrimMode::Both) const;

    ErrorOr<Vector<String>> split_limit(u32 separator, size_t limit, SplitBehavior = SplitBehavior::Nothing) const;
    ErrorOr<Vector<String>> split(u32 separator, SplitBehavior = SplitBehavior::Nothing) const;

    Optional<size_t> find_byte_offset(u32 code_point, size_t from_byte_offset = 0) const;
    Optional<size_t> find_byte_offset(StringView substring, size_t from_byte_offset = 0) const;

    // Using this method requires linking LibUnicode into your application.
    Optional<size_t> find_byte_offset_ignoring_case(StringView, size_t from_byte_offset = 0) const;

    [[nodiscard]] bool operator==(String const&) const = default;
    [[nodiscard]] bool operator==(FlyString const&) const;
    [[nodiscard]] bool operator==(StringView) const;
    [[nodiscard]] bool operator==(char const* cstring) const;

    // NOTE: UTF-8 is defined in a way that lexicographic ordering of code points is equivalent to lexicographic ordering of bytes.
    [[nodiscard]] int operator<=>(String const& other) const { return this->bytes_as_string_view().compare(other.bytes_as_string_view()); }

    template<typename... Ts>
    [[nodiscard]] ALWAYS_INLINE constexpr bool is_one_of(Ts&&... strings) const
    {
        return (this->operator==(forward<Ts>(strings)) || ...);
    }

    [[nodiscard]] bool contains(StringView, CaseSensitivity = CaseSensitivity::CaseSensitive) const;
    [[nodiscard]] bool contains(u32, CaseSensitivity = CaseSensitivity::CaseSensitive) const;

    [[nodiscard]] u32 ascii_case_insensitive_hash() const;

    template<Integral T>
    [[nodiscard]] static String number(T);

    template<FloatingPoint T>
    [[nodiscard]] static String number(T value)
    {
        return MUST(formatted("{}", value));
    }

    template<Arithmetic T>
    Optional<T> to_number(TrimWhitespace trim_whitespace = TrimWhitespace::Yes) const
    {
        return bytes_as_string_view().to_number<T>(trim_whitespace);
    }

    static ErrorOr<String> vformatted(StringView fmtstr, TypeErasedFormatParams&);

    template<typename... Parameters>
    static ErrorOr<String> formatted(CheckedFormatString<Parameters...>&& fmtstr, Parameters const&... parameters)
    {
        VariadicFormatParams<AllowDebugOnlyFormatters::No, Parameters...> variadic_format_parameters { parameters... };
        return vformatted(fmtstr.view(), variadic_format_parameters);
    }

    template<class SeparatorType, class CollectionType>
    static ErrorOr<String> join(SeparatorType const& separator, CollectionType const& collection, StringView fmtstr = "{}"sv)
    {
        StringBuilder builder;
        TRY(builder.try_join(separator, collection, fmtstr));
        return builder.to_string();
    }

    // FIXME: Remove these once all code has been ported to String
    [[nodiscard]] ByteString to_byte_string() const;
    static ErrorOr<String> from_byte_string(ByteString const&);
    template<typename T>
    requires(IsSame<RemoveCVReference<T>, StringView>)
    static ErrorOr<String> from_byte_string(T&&) = delete;

private:
    friend class ::AK::FlyString;
    friend class Optional<String>;

    using ShortString = Detail::ShortString;

    constexpr bool is_invalid() const
    {
        return raw(Badge<String> {}) == 0;
    }

    explicit constexpr String(StringBase&& base)
        : StringBase(move(base))
    {
    }

    explicit constexpr String(nullptr_t)
        : StringBase(nullptr)
    {
    }
};

template<>
class Optional<String> : public OptionalBase<String> {
    template<typename U>
    friend class Optional;

public:
    using ValueType = String;

    constexpr Optional() = default;

    template<SameAs<OptionalNone> V>
    constexpr Optional(V) { }

    constexpr Optional(Optional<String> const& other)
    {
        if (other.has_value())
            m_value = other.m_value;
    }

    constexpr Optional(Optional&& other)
        : m_value(move(other.m_value))
    {
    }

    template<typename U = String>
    requires(!IsSame<OptionalNone, RemoveCVReference<U>>)
    explicit(!IsConvertible<U&&, String>) constexpr Optional(U&& value)
    requires(!IsSame<RemoveCVReference<U>, Optional<String>> && IsConstructible<String, U &&>)
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
            m_value = other.m_value;
        }
        return *this;
    }

    constexpr Optional& operator=(Optional&& other)
    {
        if (this != &other) {
            m_value = move(other.m_value);
        }
        return *this;
    }

    constexpr void clear()
    {
        m_value = String(nullptr);
    }

    [[nodiscard]] constexpr bool has_value() const
    {
        return !m_value.is_invalid();
    }

    [[nodiscard]] constexpr String& value() &
    {
        VERIFY(has_value());
        return m_value;
    }

    [[nodiscard]] constexpr String const& value() const&
    {
        VERIFY(has_value());
        return m_value;
    }

    [[nodiscard]] constexpr String value() &&
    {
        return release_value();
    }

    [[nodiscard]] constexpr String release_value()
    {
        VERIFY(has_value());
        String released_value = move(m_value);
        clear();
        return released_value;
    }

private:
    String m_value { nullptr };
};

template<>
struct Traits<String> : public DefaultTraits<String> {
    static unsigned hash(String const&);
};

template<>
struct Formatter<String> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder&, String const&);
};

struct ASCIICaseInsensitiveStringTraits : public Traits<String> {
    static unsigned hash(String const& s) { return s.ascii_case_insensitive_hash(); }
    static bool equals(String const& a, String const& b) { return a.bytes_as_string_view().equals_ignoring_ascii_case(b.bytes_as_string_view()); }
};

}

[[nodiscard]] ALWAYS_INLINE AK::String operator""_string(char const* cstring, size_t length)
{
    ASSERT(Utf8View(AK::StringView(cstring, length)).validate());
    return AK::String::from_utf8_without_validation({ cstring, length });
}
