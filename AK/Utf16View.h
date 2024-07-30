/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Endian.h>
#include <AK/Error.h>
#include <AK/Format.h>
#include <AK/Forward.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <AK/Vector.h>

namespace AK {

using Utf16Data = Vector<u16, 1>;

ErrorOr<Utf16Data> utf8_to_utf16(StringView, Endianness = Endianness::Host);
ErrorOr<Utf16Data> utf8_to_utf16(Utf8View const&, Endianness = Endianness::Host);
ErrorOr<Utf16Data> utf32_to_utf16(Utf32View const&, Endianness = Endianness::Host);
ErrorOr<void> code_point_to_utf16(Utf16Data&, u32, Endianness = Endianness::Host);

size_t utf16_code_unit_length_from_utf8(StringView);

class Utf16View;

class Utf16CodePointIterator {
    friend class Utf16View;

public:
    Utf16CodePointIterator() = default;
    ~Utf16CodePointIterator() = default;

    bool operator==(Utf16CodePointIterator const& other) const
    {
        return (m_ptr == other.m_ptr) && (m_remaining_code_units == other.m_remaining_code_units);
    }

    Utf16CodePointIterator& operator++();
    u32 operator*() const;

    size_t length_in_code_units() const;

private:
    Utf16CodePointIterator(u16 const* ptr, size_t length, Endianness endianness)
        : m_ptr(ptr)
        , m_remaining_code_units(length)
        , m_endianness(endianness)
    {
    }

    u16 const* m_ptr { nullptr };
    size_t m_remaining_code_units { 0 };
    Endianness m_endianness { Endianness::Host };
};

class Utf16View {
public:
    using Iterator = Utf16CodePointIterator;

    static bool is_high_surrogate(u16);
    static bool is_low_surrogate(u16);
    static u32 decode_surrogate_pair(u16 high_surrogate, u16 low_surrogate);

    Utf16View() = default;
    ~Utf16View() = default;

    explicit Utf16View(ReadonlySpan<u16> code_units, Endianness endianness = Endianness::Host)
        : m_code_units(code_units)
        , m_endianness(endianness)
    {
    }

    template<size_t Size>
    Utf16View(char16_t const (&code_units)[Size], Endianness endianness = Endianness::Host)
        : m_code_units(
              reinterpret_cast<u16 const*>(&code_units[0]),
              code_units[Size - 1] == u'\0' ? Size - 1 : Size)
        , m_endianness(endianness)
    {
    }

    bool operator==(Utf16View const& other) const { return m_code_units == other.m_code_units; }

    enum class AllowInvalidCodeUnits {
        Yes,
        No,
    };

    ErrorOr<ByteString> to_byte_string(AllowInvalidCodeUnits = AllowInvalidCodeUnits::No) const;
    ErrorOr<String> to_utf8(AllowInvalidCodeUnits = AllowInvalidCodeUnits::No) const;

    bool is_null() const { return m_code_units.is_null(); }
    bool is_empty() const { return m_code_units.is_empty(); }
    size_t length_in_code_units() const { return m_code_units.size(); }
    size_t length_in_code_points() const;

    Endianness endianness() const { return m_endianness; }

    Utf16CodePointIterator begin() const { return { begin_ptr(), m_code_units.size(), m_endianness }; }
    Utf16CodePointIterator end() const { return { end_ptr(), 0, m_endianness }; }

    u16 const* data() const { return m_code_units.data(); }
    char16_t const* char_data() const { return reinterpret_cast<char16_t const*>(data()); }

    u16 code_unit_at(size_t index) const;
    u32 code_point_at(size_t index) const;

    size_t code_point_offset_of(size_t code_unit_offset) const;
    size_t code_unit_offset_of(size_t code_point_offset) const;
    size_t code_unit_offset_of(Utf16CodePointIterator const&) const;

    Utf16View substring_view(size_t code_unit_offset, size_t code_unit_length) const;
    Utf16View substring_view(size_t code_unit_offset) const { return substring_view(code_unit_offset, length_in_code_units() - code_unit_offset); }

    Utf16View unicode_substring_view(size_t code_point_offset, size_t code_point_length) const;
    Utf16View unicode_substring_view(size_t code_point_offset) const { return unicode_substring_view(code_point_offset, length_in_code_points() - code_point_offset); }

    bool starts_with(Utf16View const&) const;

    bool validate() const;
    bool validate(size_t& valid_code_units) const;

    bool equals_ignoring_case(Utf16View const&) const;

private:
    u16 const* begin_ptr() const { return m_code_units.data(); }
    u16 const* end_ptr() const { return begin_ptr() + m_code_units.size(); }

    size_t calculate_length_in_code_points() const;

    ReadonlySpan<u16> m_code_units;
    mutable Optional<size_t> m_length_in_code_points;
    Endianness m_endianness { Endianness::Host };
};

}

template<>
struct AK::Formatter<AK::Utf16View> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, AK::Utf16View const& value)
    {
        return builder.builder().try_append(value);
    }
};

#if USING_AK_GLOBALLY
using AK::Utf16Data;
using AK::Utf16View;
#endif
