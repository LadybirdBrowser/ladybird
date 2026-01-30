/*
 * Copyright (c) 2021-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Format.h>
#include <AK/Forward.h>
#include <AK/IterationDecision.h>
#include <AK/MemMem.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <AK/StringConversions.h>
#include <AK/StringHash.h>
#include <AK/Traits.h>
#include <AK/Types.h>
#include <AK/UnicodeUtils.h>
#include <AK/Utf8View.h>
#include <AK/Vector.h>

namespace AK {

[[nodiscard]] bool validate_utf16_le(ReadonlyBytes);
[[nodiscard]] bool validate_utf16_be(ReadonlyBytes);

namespace Detail {

static constexpr inline auto UTF16_FLAG = NumericLimits<size_t>::digits() - 1;
class Utf16StringBase;

}

class Utf16CodePointIterator {
    friend class Utf16View;

public:
    Utf16CodePointIterator() = default;
    ~Utf16CodePointIterator() = default;

    constexpr Utf16CodePointIterator& operator++()
    {
        auto remaining_code_units = this->remaining_code_units();
        VERIFY(remaining_code_units > 0);

        if (has_ascii_storage()) {
            ++m_iterator.ascii;
            --m_remaining_code_units;
        } else {
            auto length = min(length_in_code_units(), remaining_code_units);

            m_iterator.utf16 += length;
            m_remaining_code_units -= length;
        }

        return *this;
    }

    constexpr u32 operator*() const
    {
        auto remaining_code_units = this->remaining_code_units();
        VERIFY(remaining_code_units > 0);

        if (has_ascii_storage())
            return *m_iterator.ascii;

        auto code_unit = *m_iterator.utf16;

        if (UnicodeUtils::is_utf16_high_surrogate(code_unit)) {
            if (remaining_code_units > 1) {
                auto next_code_unit = *(m_iterator.utf16 + 1);

                if (UnicodeUtils::is_utf16_low_surrogate(next_code_unit))
                    return UnicodeUtils::decode_utf16_surrogate_pair(code_unit, next_code_unit);
            }
        }

        return static_cast<u32>(code_unit);
    }

    constexpr Optional<u32> peek(size_t code_point_offset) const
    {
        if (code_point_offset == 0) {
            if (remaining_code_units() == 0)
                return {};
            return this->operator*();
        }

        auto it = *this;

        for (size_t index = 0; index < code_point_offset; ++index) {
            ++it;
            if (it.remaining_code_units() == 0)
                return {};
        }

        return *it;
    }

    [[nodiscard]] constexpr bool operator==(Utf16CodePointIterator const& other) const
    {
        // Note that this also protects against iterators with different underlying storage.
        if (m_remaining_code_units != other.m_remaining_code_units)
            return false;

        if (has_ascii_storage())
            return m_iterator.ascii == other.m_iterator.ascii;
        return m_iterator.utf16 == other.m_iterator.utf16;
    }

    [[nodiscard]] constexpr size_t length_in_code_units()
    {
        if (has_ascii_storage())
            return 1;
        return UnicodeUtils::code_unit_length_for_code_point(**this);
    }

private:
    friend Utf16View;

    constexpr Utf16CodePointIterator(char const* iterator, size_t length)
        : m_iterator { .ascii = iterator }
        , m_remaining_code_units(length)
    {
    }

    constexpr Utf16CodePointIterator(char16_t const* iterator, size_t length)
        : m_iterator { .utf16 = iterator }
        , m_remaining_code_units(length)
    {
        m_remaining_code_units |= 1uz << Detail::UTF16_FLAG;
    }

    constexpr bool has_ascii_storage() const { return m_remaining_code_units >> Detail::UTF16_FLAG == 0; }
    constexpr size_t remaining_code_units() const { return m_remaining_code_units & ~(1uz << Detail::UTF16_FLAG); }

    union {
        char const* ascii;
        char16_t const* utf16;
    } m_iterator { .ascii = nullptr };

    // Just like Utf16StringData, we store whether this string has ASCII or UTF-16 storage by setting the most
    // significant bit of m_remaining_code_units for UTF-16 storage.
    size_t m_remaining_code_units { 0 };
};

class Utf16View {
public:
    using Iterator = Utf16CodePointIterator;

    Utf16View() = default;
    ~Utf16View() = default;

    constexpr Utf16View(char16_t const* string, size_t length_in_code_units)
        : m_string { .utf16 = string }
        , m_length_in_code_units(length_in_code_units)
    {
        m_length_in_code_units |= 1uz << Detail::UTF16_FLAG;
    }

    consteval Utf16View(StringView string)
        : m_string { .ascii = string.characters_without_null_termination() }
        , m_length_in_code_units(string.length())
    {
        VERIFY(all_of(string, AK::is_ascii));
    }

    ErrorOr<String> to_utf8(AllowLonelySurrogates = AllowLonelySurrogates::Yes) const;
    ErrorOr<ByteString> to_byte_string(AllowLonelySurrogates = AllowLonelySurrogates::Yes) const;

    ALWAYS_INLINE String to_utf8_but_should_be_ported_to_utf16(AllowLonelySurrogates allow_lonely_surrogates = AllowLonelySurrogates::Yes) const
    {
        return MUST(to_utf8(allow_lonely_surrogates));
    }

    Utf16String to_ascii_lowercase() const;
    Utf16String to_ascii_uppercase() const;
    Utf16String to_ascii_titlecase() const;

    [[nodiscard]] constexpr bool has_ascii_storage() const { return m_length_in_code_units >> Detail::UTF16_FLAG == 0; }

    [[nodiscard]] ALWAYS_INLINE ReadonlyBytes bytes() const
    {
        VERIFY(has_ascii_storage());
        return { m_string.ascii, length_in_code_units() };
    }

    [[nodiscard]] constexpr ReadonlySpan<char> ascii_span() const
    {
        VERIFY(has_ascii_storage());
        return { m_string.ascii, length_in_code_units() };
    }

    [[nodiscard]] constexpr ReadonlySpan<char16_t> utf16_span() const
    {
        VERIFY(!has_ascii_storage());
        return { m_string.utf16, length_in_code_units() };
    }

    template<Arithmetic T>
    ALWAYS_INLINE Optional<T> to_number(TrimWhitespace trim_whitespace = TrimWhitespace::Yes, int base = 10) const
    {
        if (has_ascii_storage())
            return parse_number<T>(bytes(), trim_whitespace, base);
        return parse_number<T>(*this, trim_whitespace, base);
    }

    [[nodiscard]] constexpr bool operator==(Utf16View const& other) const
    {
        if (length_in_code_units() != other.length_in_code_units())
            return false;

        if (has_ascii_storage() && other.has_ascii_storage())
            return TypedTransfer<char>::compare(m_string.ascii, other.m_string.ascii, length_in_code_units());
        if (!has_ascii_storage() && !other.has_ascii_storage())
            return TypedTransfer<char16_t>::compare(m_string.utf16, other.m_string.utf16, length_in_code_units());

        for (size_t i = 0; i < length_in_code_units(); ++i) {
            if (code_unit_at(i) != other.code_unit_at(i))
                return false;
        }

        return true;
    }

    [[nodiscard]] constexpr bool operator==(StringView other) const
    {
        if (has_ascii_storage())
            return StringView { m_string.ascii, length_in_code_units() } == other;

        if (other.is_ascii())
            return *this == Utf16View { other.characters_without_null_termination(), other.length() };

        Utf8View other_utf8 { other };

        auto this_it = begin();
        auto other_it = other_utf8.begin();

        for (; this_it != end() && other_it != other_utf8.end(); ++this_it, ++other_it) {
            if (*this_it != *other_it)
                return false;
        }

        return this_it == end() && other_it == other_utf8.end();
    }

    [[nodiscard]] constexpr int operator<=>(Utf16View const& other) const
    {
        if (is_empty() && other.is_empty())
            return 0;

        size_t length = min(length_in_code_units(), other.length_in_code_units());
        int result = 0;

        if (has_ascii_storage() && other.has_ascii_storage()) {
            result = __builtin_memcmp(m_string.ascii, other.m_string.ascii, length);
        } else if (!has_ascii_storage() && !other.has_ascii_storage()) {
            result = __builtin_memcmp(m_string.utf16, other.m_string.utf16, length * sizeof(char16_t));
        } else {
            for (size_t i = 0; i < length; ++i) {
                auto this_code_unit = code_unit_at(i);
                auto other_code_unit = other.code_unit_at(i);

                if (this_code_unit < other_code_unit) {
                    result = -1;
                    break;
                }
                if (this_code_unit > other_code_unit) {
                    result = 1;
                    break;
                }
            }
        }

        if (result == 0) {
            if (length_in_code_units() == other.length_in_code_units())
                return 0;
            if (length_in_code_units() < other.length_in_code_units())
                return -1;
            return 1;
        }

        return result;
    }

    [[nodiscard]] constexpr bool equals_ignoring_case(Utf16View const& other) const
    {
        // FIXME: Handle non-ASCII case insensitive comparisons.
        return equals_ignoring_ascii_case(other);
    }

    [[nodiscard]] constexpr bool equals_ignoring_ascii_case(Utf16View const& other) const
    {
        if (length_in_code_units() != other.length_in_code_units())
            return false;

        for (size_t i = 0; i < length_in_code_units(); ++i) {
            if (AK::to_ascii_lowercase(code_unit_at(i)) != AK::to_ascii_lowercase(other.code_unit_at(i)))
                return false;
        }

        return true;
    }

    template<typename... Ts>
    [[nodiscard]] constexpr bool is_one_of(Ts&&... strings) const
    {
        return (this->operator==(forward<Ts>(strings)) || ...);
    }

    template<typename... Ts>
    [[nodiscard]] constexpr bool is_one_of_ignoring_ascii_case(Ts&&... strings) const
    {
        return (this->equals_ignoring_ascii_case(forward<Ts>(strings)) || ...);
    }

    [[nodiscard]] constexpr u32 hash() const
    {
        if (is_empty())
            return 0;
        if (has_ascii_storage())
            return string_hash(m_string.ascii, length_in_code_units());
        return string_hash(m_string.utf16, length_in_code_units());
    }

    [[nodiscard]] constexpr bool is_null() const
    {
        if (has_ascii_storage())
            return m_string.ascii == nullptr;
        return m_string.utf16 == nullptr;
    }

    [[nodiscard]] constexpr bool is_empty() const { return length_in_code_units() == 0; }

    [[nodiscard]] bool is_ascii() const;

    [[nodiscard]] constexpr bool is_ascii_whitespace() const
    {
        if (has_ascii_storage())
            return all_of(ascii_span(), AK::is_ascii_space);
        return all_of(utf16_span(), AK::is_ascii_space);
    }

    // Note that these do not allow lonely surrogates. The string may be assumed to always be valid WTF-16.
    [[nodiscard]] bool validate() const;
    [[nodiscard]] bool validate(size_t& valid_code_units) const;

    [[nodiscard]] constexpr size_t length_in_code_units() const { return m_length_in_code_units & ~(1uz << Detail::UTF16_FLAG); }

    [[nodiscard]] ALWAYS_INLINE size_t length_in_code_points() const
    {
        if (has_ascii_storage())
            return m_length_in_code_units;

        if (m_length_in_code_points == NumericLimits<size_t>::max())
            m_length_in_code_points = calculate_length_in_code_points();
        return m_length_in_code_points;
    }

    [[nodiscard]] constexpr char16_t code_unit_at(size_t index) const
    {
        VERIFY(index < length_in_code_units());

        if (has_ascii_storage())
            return m_string.ascii[index];
        return m_string.utf16[index];
    }

    [[nodiscard]] constexpr u32 code_point_at(size_t index) const
    {
        VERIFY(index < length_in_code_units());
        u32 code_point = code_unit_at(index);

        if (!UnicodeUtils::is_utf16_high_surrogate(code_point) && !UnicodeUtils::is_utf16_low_surrogate(code_point))
            return code_point;
        if (UnicodeUtils::is_utf16_low_surrogate(code_point) || (index + 1 == length_in_code_units()))
            return code_point;

        auto second = code_unit_at(index + 1);
        if (!UnicodeUtils::is_utf16_low_surrogate(second))
            return code_point;

        return UnicodeUtils::decode_utf16_surrogate_pair(code_point, second);
    }

    [[nodiscard]] constexpr u32 previous_code_point_at(size_t& index) const
    {
        VERIFY(index > 0);
        VERIFY(index <= length_in_code_units());
        --index;
        if (index > 0 && UnicodeUtils::is_utf16_low_surrogate(code_unit_at(index)) && UnicodeUtils::is_utf16_high_surrogate(code_unit_at(index - 1)))
            --index;
        return code_point_at(index);
    }

    [[nodiscard]] size_t code_unit_offset_of(size_t code_point_offset) const;
    [[nodiscard]] size_t code_point_offset_of(size_t code_unit_offset) const;

    [[nodiscard]] constexpr Utf16CodePointIterator begin() const
    {
        if (has_ascii_storage())
            return { m_string.ascii, length_in_code_units() };
        return { m_string.utf16, length_in_code_units() };
    }

    [[nodiscard]] constexpr Utf16CodePointIterator end() const
    {
        if (has_ascii_storage())
            return { m_string.ascii + length_in_code_units(), 0 };
        return { m_string.utf16 + length_in_code_units(), 0 };
    }

    [[nodiscard]] constexpr size_t iterator_offset(Utf16CodePointIterator const& it) const
    {
        if (has_ascii_storage()) {
            VERIFY(it.has_ascii_storage());
            VERIFY(it.m_iterator.ascii >= m_string.ascii);
            VERIFY(it.m_iterator.ascii <= m_string.ascii + length_in_code_units());

            return it.m_iterator.ascii - m_string.ascii;
        }

        VERIFY(!it.has_ascii_storage());
        VERIFY(it.m_iterator.utf16 >= m_string.utf16);
        VERIFY(it.m_iterator.utf16 <= m_string.utf16 + length_in_code_units());

        return it.m_iterator.utf16 - m_string.utf16;
    }

    [[nodiscard]] constexpr Utf16CodePointIterator iterator_at_code_unit_offset(size_t code_unit_offset) const
    {
        VERIFY(code_unit_offset <= length_in_code_units());

        if (has_ascii_storage())
            return { m_string.ascii + code_unit_offset, length_in_code_units() - code_unit_offset };
        return { m_string.utf16 + code_unit_offset, length_in_code_units() - code_unit_offset };
    }

    Utf16String replace(char16_t needle, Utf16View const& replacement, ReplaceMode) const;
    Utf16String replace(Utf16View const& needle, Utf16View const& replacement, ReplaceMode) const;
    Utf16String escape_html_entities() const;

    [[nodiscard]] constexpr Utf16View substring_view(size_t code_unit_offset, size_t code_unit_length) const
    {
        VERIFY(code_unit_offset + code_unit_length <= length_in_code_units());

        if (has_ascii_storage())
            return { m_string.ascii + code_unit_offset, code_unit_length };
        return { m_string.utf16 + code_unit_offset, code_unit_length };
    }

    [[nodiscard]] constexpr Utf16View substring_view(size_t code_unit_offset) const { return substring_view(code_unit_offset, length_in_code_units() - code_unit_offset); }

    [[nodiscard]] Utf16View unicode_substring_view(size_t code_point_offset, size_t code_point_length) const;
    [[nodiscard]] Utf16View unicode_substring_view(size_t code_point_offset) const { return unicode_substring_view(code_point_offset, length_in_code_points() - code_point_offset); }

    [[nodiscard]] constexpr Utf16View trim(Utf16View const& code_units, TrimMode mode = TrimMode::Both) const
    {
        size_t substring_start = 0;
        size_t substring_length = length_in_code_units();

        if (mode == TrimMode::Left || mode == TrimMode::Both) {
            for (size_t i = 0; i < length_in_code_units(); ++i) {
                if (substring_length == 0)
                    return {};
                if (!code_units.contains(code_unit_at(i)))
                    break;

                ++substring_start;
                --substring_length;
            }
        }

        if (mode == TrimMode::Right || mode == TrimMode::Both) {
            for (size_t i = length_in_code_units(); i > 0; --i) {
                if (substring_length == 0)
                    return {};
                if (!code_units.contains(code_unit_at(i - 1)))
                    break;

                --substring_length;
            }
        }

        return substring_view(substring_start, substring_length);
    }

    [[nodiscard]] constexpr Utf16View trim_ascii_whitespace(TrimMode mode = TrimMode::Both) const
    {
        return trim(" \n\t\v\f\r"sv, mode);
    }

    Optional<size_t> find_code_unit_offset(char16_t needle, size_t start_offset = 0) const;
    Optional<size_t> find_last_code_point_offset(u32 needle, size_t end_offset = NumericLimits<size_t>::max()) const;

    constexpr Optional<size_t> find_code_unit_offset(Utf16View const& needle, size_t start_offset = 0) const
    {
        if (has_ascii_storage() && needle.has_ascii_storage())
            return ascii_span().index_of(needle.ascii_span(), start_offset);
        if (!has_ascii_storage() && !needle.has_ascii_storage())
            return utf16_span().index_of(needle.utf16_span(), start_offset);

        Checked maximum_offset { start_offset };
        maximum_offset += needle.length_in_code_units();
        if (maximum_offset.has_overflow() || maximum_offset.value() > length_in_code_units())
            return {};

        if (needle.is_empty())
            return start_offset;

        for (size_t index = start_offset; index <= length_in_code_units() - needle.length_in_code_units();) {
            auto slice = substring_view(index, needle.length_in_code_units());
            if (slice == needle)
                return index;

            index += slice.begin().length_in_code_units();
        }

        return {};
    }

    constexpr Optional<size_t> find_code_unit_offset_ignoring_case(Utf16View const& needle, size_t start_offset = 0) const
    {
        Checked maximum_offset { start_offset };
        maximum_offset += needle.length_in_code_units();
        if (maximum_offset.has_overflow() || maximum_offset.value() > length_in_code_units())
            return {};

        if (needle.is_empty())
            return start_offset;

        size_t index = start_offset;
        while (index <= length_in_code_units() - needle.length_in_code_units()) {
            auto slice = substring_view(index, needle.length_in_code_units());
            if (slice.equals_ignoring_case(needle))
                return index;

            index += slice.begin().length_in_code_units();
        }

        return {};
    }

    [[nodiscard]] constexpr bool contains(char16_t needle) const { return find_code_unit_offset(needle).has_value(); }
    [[nodiscard]] constexpr bool contains(Utf16View const& needle) const { return find_code_unit_offset(needle).has_value(); }

    [[nodiscard]] constexpr bool contains_any_of(ReadonlySpan<u32> needles) const
    {
        for (auto code_point : *this) {
            if (needles.contains_slow(code_point))
                return true;
        }

        return false;
    }

    [[nodiscard]] constexpr size_t count(Utf16View const& needle) const
    {
        if (needle.is_empty())
            return length_in_code_units();

        size_t count = 0;
        for (size_t i = 0; i < length_in_code_units() - needle.length_in_code_units() + 1; ++i) {
            if (substring_view(i).starts_with(needle))
                ++count;
        }

        return count;
    }

    [[nodiscard]] constexpr bool starts_with(char16_t needle) const
    {
        if (is_empty())
            return false;
        return code_unit_at(0) == needle;
    }

    [[nodiscard]] constexpr bool starts_with(Utf16View const& needle) const
    {
        auto needle_length = needle.length_in_code_units();
        if (needle_length == 0)
            return true;
        if (is_empty() || needle_length > length_in_code_units())
            return false;

        return substring_view(0, needle_length) == needle;
    }

    [[nodiscard]] constexpr bool ends_with(char16_t needle) const
    {
        if (is_empty())
            return false;
        return code_unit_at(length_in_code_units() - 1) == needle;
    }

    [[nodiscard]] constexpr bool ends_with(Utf16View const& needle) const
    {
        auto needle_length = needle.length_in_code_units();
        if (needle_length == 0)
            return true;
        if (is_empty() || needle_length > length_in_code_units())
            return false;

        return substring_view(length_in_code_units() - needle_length, needle_length) == needle;
    }

    [[nodiscard]] Vector<Utf16View> split_view(char16_t, SplitBehavior) const;
    [[nodiscard]] Vector<Utf16View> split_view(Utf16View const&, SplitBehavior) const;

    template<typename Callback>
    constexpr void for_each_split_view(char16_t separator, SplitBehavior split_behavior, Callback&& callback) const
    {
        Utf16View separator_view { &separator, 1 };
        for_each_split_view(separator_view, split_behavior, forward<Callback>(callback));
    }

    template<typename Callback>
    constexpr void for_each_split_view(Utf16View const& separator, SplitBehavior split_behavior, Callback&& callback) const
    {
        VERIFY(!separator.is_empty());

        if (is_empty())
            return;

        bool keep_empty = has_flag(split_behavior, SplitBehavior::KeepEmpty);
        bool keep_separator = has_flag(split_behavior, SplitBehavior::KeepTrailingSeparator);

        auto view { *this };

        for (auto index = view.find_code_unit_offset(separator); index.has_value(); index = view.find_code_unit_offset(separator)) {
            if (keep_empty || *index > 0) {
                auto part = keep_separator
                    ? view.substring_view(0, *index + separator.length_in_code_units())
                    : view.substring_view(0, *index);

                if (callback(part) == IterationDecision::Break)
                    return;
            }

            view = view.substring_view(*index + separator.length_in_code_units());
        }

        if (keep_empty || !view.is_empty())
            callback(view);
    }

    // https://infra.spec.whatwg.org/#code-unit-less-than
    [[nodiscard]] constexpr bool is_code_unit_less_than(Utf16View const& other) const
    {
        auto common_length = min(length_in_code_units(), other.length_in_code_units());

        for (size_t position = 0; position < common_length; ++position) {
            auto this_code_unit = code_unit_at(position);
            auto other_code_unit = other.code_unit_at(position);

            if (this_code_unit != other_code_unit)
                return this_code_unit < other_code_unit;
        }

        return length_in_code_units() < other.length_in_code_units();
    }

private:
    friend StringBuilder;
    friend Detail::Utf16StringBase;
    friend Detail::Utf16StringData;

    constexpr Utf16View(char const* string, size_t length_in_code_units)
        : m_string { .ascii = string }
        , m_length_in_code_units(length_in_code_units)
    {
    }

    [[nodiscard]] size_t calculate_length_in_code_points() const;

    union {
        char const* ascii;
        char16_t const* utf16;
    } m_string { .ascii = nullptr };

    // Just like Utf16StringData, we store whether this string has ASCII or UTF-16 storage by setting the most
    // significant bit of m_length_in_code_units for UTF-16 storage.
    size_t m_length_in_code_units { 0 };
    mutable size_t m_length_in_code_points { NumericLimits<size_t>::max() };
};

template<>
struct Formatter<Utf16View> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Utf16View const& value)
    {
        return builder.builder().try_append(value);
    }
};

template<>
struct Traits<Utf16View> : public DefaultTraits<Utf16View> {
    using PeekType = Utf16View;
    using ConstPeekType = Utf16View;
    static unsigned hash(Utf16View const& s) { return s.hash(); }
};

namespace Detail {

template<>
inline constexpr bool IsHashCompatible<Utf16View, Utf16String> = true;

template<>
inline constexpr bool IsHashCompatible<Utf16String, Utf16View> = true;

}

}

[[nodiscard]] ALWAYS_INLINE consteval AK::Utf16View operator""sv(char16_t const* string, size_t length)
{
    return { string, length };
}

#if USING_AK_GLOBALLY
using AK::Utf16View;
#endif
