/*
 * Copyright (c) 2024, Jonne Ransijn <jonne@yyny.dev>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Checked.h>
#include <AK/Concepts.h>
#include <AK/EnumBits.h>
#include <AK/Forward.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/StdLibExtras.h>
#include <AK/StringHash.h>
#include <AK/StringUtils.h>
#include <AK/UnicodeCodePointView.h>
#include <AK/Utf8View.h>

namespace AK {

class AsciiStringView : public UnicodeCodePointViewBase<AsciiStringView, AsciiChar, Utf8View> {
public:
    constexpr AsciiStringView() = default;

    template<size_t N>
    ALWAYS_INLINE consteval AsciiStringView(char const (&characters)[N])
        : UnicodeCodePointViewBase(characters, N, N)
    {
        if (!is_constant_evaluated())
            VERIFY(!Checked<uintptr_t>::addition_would_overflow(reinterpret_cast<uintptr_t>(characters), N));
        for (size_t i = 0; i < N; i++)
            VERIFY(characters[i] >= 0x00 && characters[i] <= 0x7f);
    }

    ALWAYS_INLINE constexpr AsciiStringView(char const* characters, size_t length)
        : UnicodeCodePointViewBase(characters, length, length)
    {
        if (!is_constant_evaluated())
            VERIFY(!Checked<uintptr_t>::addition_would_overflow(reinterpret_cast<uintptr_t>(characters), length));
        for (size_t i = 0; i < length; i++)
            VERIFY(characters[i] >= 0x00 && characters[i] <= 0x7f);
    }

    ALWAYS_INLINE constexpr AsciiStringView(AsciiChar const* characters, size_t length)
        : UnicodeCodePointViewBase(bit_cast<char const*>(characters), length, length)
    {
        if (!is_constant_evaluated())
            VERIFY(!Checked<uintptr_t>::addition_would_overflow(reinterpret_cast<uintptr_t>(characters), length));
    }

    Optional<UnicodeCodePoint> chomp_one_left() &
    {
        if (m_code_unit_length == 0)
            return {};
        auto const* code_units = reinterpret_cast<AsciiChar const*>(m_code_units);
        auto result = code_units[0];
        m_code_units = code_units + 1;
        --*m_code_point_length;
        --m_code_unit_length;
        return result;
    }

    Optional<UnicodeCodePoint> chomp_one_right() &
    {
        if (m_code_unit_length == 0)
            return {};
        auto const* code_units = reinterpret_cast<AsciiChar const*>(m_code_units);
        --*m_code_point_length;
        return code_units[--m_code_unit_length];
    }

    constexpr AsciiChar const& operator[](size_t index) const
    {
        if (!is_constant_evaluated())
            VERIFY(index < m_code_unit_length);
        return reinterpret_cast<AsciiChar const&>(characters_without_null_termination()[index]);
    }

    [[nodiscard]] constexpr unsigned hash() const
    {
        if (is_empty())
            return 0;
        return string_hash(characters_without_null_termination(), length());
    }

    char const* characters_without_null_termination() const
    {
        return bit_cast<char const*>(m_code_units);
    }

    size_t size() const
    {
        return m_code_unit_length;
    }

private:
    using UnicodeCodePointViewBase::UnicodeCodePointViewBase;
};

template<>
struct Traits<AsciiStringView> : public DefaultTraits<AsciiStringView> {
    static unsigned hash(AsciiStringView s) { return s.hash(); }
};

struct CaseInsensitiveASCIIAsciiStringViewTraits : public Traits<AsciiStringView> {
    static unsigned hash(AsciiStringView s)
    {
        if (s.is_empty())
            return 0;
        return case_insensitive_string_hash(s.characters_without_null_termination(), s.length());
    }
    static bool equals(AsciiStringView const& a, AsciiStringView const& b) { return a.equals_ignoring_ascii_case(b); }
};

}

// FIXME: Remove this when clang on BSD distributions fully support consteval (specifically in the context of default parameter initialization).
//        Note that this is fixed in clang-15, but is not yet picked up by all downstream distributions.
//        See: https://github.com/llvm/llvm-project/issues/48230
//        Additionally, oss-fuzz currently ships an llvm-project commit that is a pre-release of 15.0.0.
//        See: https://github.com/google/oss-fuzz/issues/9989
//        Android currently doesn't ship clang-15 in any NDK
#if defined(AK_OS_BSD_GENERIC) || defined(OSS_FUZZ) || defined(AK_OS_ANDROID)
#    define AK_ASCII_STRING_VIEW_LITERAL_CONSTEVAL constexpr
#else
#    define AK_ASCII_STRING_VIEW_LITERAL_CONSTEVAL consteval
#endif

[[nodiscard]] ALWAYS_INLINE AK_ASCII_STRING_VIEW_LITERAL_CONSTEVAL AK::AsciiStringView operator""_ascii(char const* cstring, size_t length)
{
    return AK::AsciiStringView(cstring, length);
}

#if USING_AK_GLOBALLY
using AK::AsciiStringView;
using AK::CaseInsensitiveASCIIAsciiStringViewTraits;
#endif
