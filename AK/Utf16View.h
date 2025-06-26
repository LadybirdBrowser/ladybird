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
#include <AK/MemMem.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <AK/StringHash.h>
#include <AK/Traits.h>
#include <AK/Types.h>
#include <AK/UnicodeUtils.h>
#include <AK/Vector.h>

namespace AK {

using Utf16Data = Vector<char16_t, 1>;

struct Utf16ConversionResult {
    Utf16Data data;
    size_t code_point_count;
};
ErrorOr<Utf16ConversionResult> utf8_to_utf16(StringView);
ErrorOr<Utf16ConversionResult> utf8_to_utf16(Utf8View const&);
ErrorOr<Utf16ConversionResult> utf32_to_utf16(Utf32View const&);

[[nodiscard]] bool validate_utf16_le(ReadonlyBytes);
[[nodiscard]] bool validate_utf16_be(ReadonlyBytes);

size_t utf16_code_unit_length_from_utf8(StringView);

class Utf16CodePointIterator {
    friend class Utf16View;

public:
    Utf16CodePointIterator() = default;
    ~Utf16CodePointIterator() = default;

    constexpr Utf16CodePointIterator& operator++()
    {
        VERIFY(m_remaining_code_units > 0);

        auto length = min(length_in_code_units(), m_remaining_code_units);
        m_iterator += length;
        m_remaining_code_units -= length;

        return *this;
    }

    constexpr u32 operator*() const
    {
        VERIFY(m_remaining_code_units > 0);
        auto code_unit = *m_iterator;

        if (UnicodeUtils::is_utf16_high_surrogate(code_unit)) {
            if (m_remaining_code_units > 1) {
                auto next_code_unit = *(m_iterator + 1);

                if (UnicodeUtils::is_utf16_low_surrogate(next_code_unit))
                    return UnicodeUtils::decode_utf16_surrogate_pair(code_unit, next_code_unit);
            }

            return UnicodeUtils::REPLACEMENT_CODE_POINT;
        }

        if (UnicodeUtils::is_utf16_low_surrogate(code_unit))
            return UnicodeUtils::REPLACEMENT_CODE_POINT;

        return static_cast<u32>(code_unit);
    }

    [[nodiscard]] constexpr bool operator==(Utf16CodePointIterator const& other) const
    {
        return (m_iterator == other.m_iterator) && (m_remaining_code_units == other.m_remaining_code_units);
    }

    [[nodiscard]] constexpr size_t length_in_code_units() const
    {
        return UnicodeUtils::code_unit_length_for_code_point(**this);
    }

private:
    Utf16CodePointIterator(char16_t const* ptr, size_t length)
        : m_iterator(ptr)
        , m_remaining_code_units(length)
    {
    }

    char16_t const* m_iterator { nullptr };
    size_t m_remaining_code_units { 0 };
};

class Utf16View {
public:
    using Iterator = Utf16CodePointIterator;

    enum class AllowInvalidCodeUnits {
        No,
        Yes,
    };

    Utf16View() = default;
    ~Utf16View() = default;

    constexpr Utf16View(char16_t const* string, size_t length_in_code_units)
        : m_string(string)
        , m_length_in_code_units(length_in_code_units)
    {
    }

    constexpr Utf16View(Utf16Data const& string)
        : m_string(string.data())
        , m_length_in_code_units(string.size())
    {
    }

    Utf16View(Utf16ConversionResult&&) = delete;
    explicit Utf16View(Utf16ConversionResult const& conversion_result)
        : m_string(conversion_result.data.data())
        , m_length_in_code_units(conversion_result.data.size())
        , m_length_in_code_points(conversion_result.code_point_count)
    {
    }

    ErrorOr<String> to_utf8(AllowInvalidCodeUnits = AllowInvalidCodeUnits::No) const;
    ErrorOr<ByteString> to_byte_string(AllowInvalidCodeUnits = AllowInvalidCodeUnits::No) const;

    [[nodiscard]] constexpr ReadonlySpan<char16_t> span() const
    {
        return { m_string, length_in_code_units() };
    }

    [[nodiscard]] constexpr bool operator==(Utf16View const& other) const
    {
        if (length_in_code_units() != other.length_in_code_units())
            return false;
        return TypedTransfer<char16_t>::compare(m_string, other.m_string, length_in_code_units());
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
            if (to_ascii_lowercase(code_unit_at(i)) != to_ascii_lowercase(other.code_unit_at(i)))
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
        return string_hash(reinterpret_cast<char const*>(m_string), length_in_code_units() * sizeof(char16_t));
    }

    [[nodiscard]] constexpr bool is_null() const { return m_string == nullptr; }
    [[nodiscard]] constexpr bool is_empty() const { return length_in_code_units() == 0; }
    [[nodiscard]] bool is_ascii() const;

    [[nodiscard]] ALWAYS_INLINE bool validate(AllowInvalidCodeUnits allow_invalid_code_units = AllowInvalidCodeUnits::No) const
    {
        size_t valid_code_units = 0;
        return validate(valid_code_units, allow_invalid_code_units);
    }

    [[nodiscard]] bool validate(size_t& valid_code_units, AllowInvalidCodeUnits = AllowInvalidCodeUnits::No) const;

    [[nodiscard]] constexpr size_t length_in_code_units() const { return m_length_in_code_units; }

    [[nodiscard]] ALWAYS_INLINE size_t length_in_code_points() const
    {
        if (m_length_in_code_points == NumericLimits<size_t>::max())
            m_length_in_code_points = calculate_length_in_code_points();
        return m_length_in_code_points;
    }

    constexpr Optional<size_t> length_in_code_points_if_known() const
    {
        if (m_length_in_code_points == NumericLimits<size_t>::max())
            return {};
        return m_length_in_code_points;
    }

    constexpr void unsafe_set_code_point_length(size_t length) const { m_length_in_code_points = length; }

    [[nodiscard]] constexpr char16_t code_unit_at(size_t index) const
    {
        VERIFY(index < length_in_code_units());
        return m_string[index];
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

    [[nodiscard]] size_t code_unit_offset_of(size_t code_point_offset) const;
    [[nodiscard]] size_t code_point_offset_of(size_t code_unit_offset) const;

    [[nodiscard]] constexpr Utf16CodePointIterator begin() const
    {
        return { m_string, length_in_code_units() };
    }

    [[nodiscard]] constexpr Utf16CodePointIterator end() const
    {
        return { m_string + length_in_code_units(), 0 };
    }

    [[nodiscard]] constexpr Utf16View substring_view(size_t code_unit_offset, size_t code_unit_length) const
    {
        VERIFY(code_unit_offset + code_unit_length <= length_in_code_units());
        return { m_string + code_unit_offset, code_unit_length };
    }

    [[nodiscard]] constexpr Utf16View substring_view(size_t code_unit_offset) const { return substring_view(code_unit_offset, length_in_code_units() - code_unit_offset); }

    [[nodiscard]] Utf16View unicode_substring_view(size_t code_point_offset, size_t code_point_length) const;
    [[nodiscard]] Utf16View unicode_substring_view(size_t code_point_offset) const { return unicode_substring_view(code_point_offset, length_in_code_points() - code_point_offset); }

    constexpr Optional<size_t> find_code_unit_offset(char16_t needle, size_t start_offset = 0) const
    {
        if (start_offset >= length_in_code_units())
            return {};
        return AK::memmem_optional(m_string + start_offset, (length_in_code_units() - start_offset) * sizeof(char16_t), &needle, sizeof(needle));
    }

    constexpr Optional<size_t> find_code_unit_offset(Utf16View const& needle, size_t start_offset = 0) const
    {
        return span().index_of(needle.span(), start_offset);
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

    [[nodiscard]] constexpr bool starts_with(Utf16View const& needle) const
    {
        if (needle.is_empty())
            return true;
        if (is_empty())
            return false;
        if (needle.length_in_code_units() > length_in_code_units())
            return false;

        if (m_string == needle.m_string)
            return true;
        return span().starts_with(needle.span());
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
    [[nodiscard]] size_t calculate_length_in_code_points() const;

    char16_t const* m_string { nullptr };
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

}

[[nodiscard]] ALWAYS_INLINE AK_STRING_VIEW_LITERAL_CONSTEVAL AK::Utf16View operator""sv(char16_t const* string, size_t length)
{
    AK::Utf16View view { string, length };
    ASSERT(view.validate());
    return view;
}

#if USING_AK_GLOBALLY
using AK::Utf16Data;
using AK::Utf16View;
#endif
