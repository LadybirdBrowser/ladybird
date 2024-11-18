/*
 * Copyright (c) 2024, Jonne Ransijn <jonne@yyny.dev>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Forward.h>
#include <AK/Noncopyable.h>

namespace AK {

// FIXME: Remove this when clang on BSD distributions fully support consteval (specifically in the context of default parameter initialization).
//        Note that this is fixed in clang-15, but is not yet picked up by all downstream distributions.
//        See: https://github.com/llvm/llvm-project/issues/48230
//        Additionally, oss-fuzz currently ships an llvm-project commit that is a pre-release of 15.0.0.
//        See: https://github.com/google/oss-fuzz/issues/9989
//        Android currently doesn't ship clang-15 in any NDK
#if defined(AK_OS_BSD_GENERIC) || defined(OSS_FUZZ) || defined(AK_OS_ANDROID)
#    define AK_ASCII_CHAR_LITERAL_CONSTEVAL constexpr
#else
#    define AK_ASCII_CHAR_LITERAL_CONSTEVAL consteval
#endif

class AsciiChar {
    AK_MAKE_DEFAULT_COPYABLE(AsciiChar);
    AK_MAKE_DEFAULT_MOVABLE(AsciiChar);

public:
    constexpr AsciiChar() = default;

    AK_ASCII_CHAR_LITERAL_CONSTEVAL AsciiChar(char const ch)
        : AsciiChar({}, ch)
    {
        VERIFY(ch >= 0x00 && ch <= 0x7f);
    }

    static constexpr AsciiChar checked(char const ch)
    {
        VERIFY(ch >= 0x00 && ch <= 0x7f);

        return AsciiChar({}, ch);
    }

    static constexpr AsciiChar unchecked(char const ch)
    {
        return AsciiChar({}, ch);
    }

    constexpr bool operator==(AsciiChar const& other) const = default;
    constexpr bool operator!=(AsciiChar const& other) const = default;

    constexpr operator char() const { return m_char; }
    constexpr operator signed char() const { return m_char; }
    constexpr operator unsigned char() const { return m_char; }
    constexpr operator u32() const { return m_char; }

private:
    constexpr AsciiChar(Badge<AsciiChar>, char const ch)
        : m_char(ch)
    {
        ASSERT(ch >= 0x00 && ch <= 0x7f);
    }

    char m_char;
};

static_assert(IsTrivial<AsciiChar>);

constexpr bool operator==(u32 lhs, AsciiChar rhs) { return lhs == static_cast<u32>(rhs); }
constexpr bool operator!=(u32 lhs, AsciiChar rhs) { return lhs != static_cast<u32>(rhs); }
constexpr bool operator==(AsciiChar lhs, u32 rhs) { return static_cast<u32>(lhs) == rhs; }
constexpr bool operator!=(AsciiChar lhs, u32 rhs) { return static_cast<u32>(lhs) != rhs; }

}

[[nodiscard]] ALWAYS_INLINE AK_ASCII_CHAR_LITERAL_CONSTEVAL AK::AsciiChar operator""_ascii(char ch)
{
    return AK::AsciiChar(ch);
}

#if USING_AK_GLOBALLY
using AK::AsciiChar;
#endif
