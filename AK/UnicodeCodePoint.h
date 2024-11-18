/*
 * Copyright (c) 2024, Jonne Ransijn <jonne@yyny.dev>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AsciiChar.h>
#include <AK/Badge.h>
#include <AK/CharacterTypes.h>
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
#    define AK_UNICODE_CODE_POINT_LITERAL_CONSTEVAL constexpr
#else
#    define AK_UNICODE_CODE_POINT_LITERAL_CONSTEVAL consteval
#endif

class UnicodeCodePoint {
public:
    constexpr UnicodeCodePoint() = default;

    constexpr UnicodeCodePoint(char) = delete;

    constexpr UnicodeCodePoint(u8 const cp)
        : UnicodeCodePoint({}, cp)
    {
    }

    constexpr UnicodeCodePoint(u16 const cp)
        : UnicodeCodePoint({}, cp)
    {
    }

    AK_UNICODE_CODE_POINT_LITERAL_CONSTEVAL UnicodeCodePoint(u32 const cp)
        : UnicodeCodePoint({}, cp)
    {
        VERIFY(is_unicode(cp));
    }

    AK_UNICODE_CODE_POINT_LITERAL_CONSTEVAL UnicodeCodePoint(char32_t) = delete;

    constexpr UnicodeCodePoint(AsciiChar const ch)
        : m_cp(ch)
    {
    }

    static constexpr UnicodeCodePoint checked(u32 cp)
    {
        VERIFY(is_unicode(cp));

        return UnicodeCodePoint({}, cp);
    }

    static constexpr UnicodeCodePoint unchecked(u32 cp)
    {
        return UnicodeCodePoint({}, cp);
    }

    constexpr bool operator==(UnicodeCodePoint const& other) const = default;
    constexpr bool operator!=(UnicodeCodePoint const& other) const = default;

    constexpr operator u32() const { return m_cp; }

    static UnicodeCodePoint const REPLACEMENT_CHARACTER;

private:
    constexpr UnicodeCodePoint(Badge<UnicodeCodePoint>, u32 cp)
        : m_cp(cp)
    {
        ASSERT(is_unicode(cp));
    }

    u32 m_cp;
};

inline constexpr UnicodeCodePoint UnicodeCodePoint::REPLACEMENT_CHARACTER = 0xFFFDu;

static_assert(IsTrivial<UnicodeCodePoint>);

constexpr bool operator==(u32 lhs, UnicodeCodePoint rhs) { return lhs == static_cast<u32>(rhs); }
constexpr bool operator!=(u32 lhs, UnicodeCodePoint rhs) { return lhs != static_cast<u32>(rhs); }
constexpr bool operator==(UnicodeCodePoint lhs, u32 rhs) { return static_cast<u32>(lhs) == rhs; }
constexpr bool operator!=(UnicodeCodePoint lhs, u32 rhs) { return static_cast<u32>(lhs) != rhs; }

constexpr bool operator==(UnicodeCodePoint lhs, AsciiChar rhs) { return lhs == UnicodeCodePoint(rhs); }
constexpr bool operator!=(UnicodeCodePoint lhs, AsciiChar rhs) { return lhs != UnicodeCodePoint(rhs); }
constexpr bool operator==(AsciiChar lhs, UnicodeCodePoint rhs) { return UnicodeCodePoint(lhs) == rhs; }
constexpr bool operator!=(AsciiChar lhs, UnicodeCodePoint rhs) { return UnicodeCodePoint(lhs) != rhs; }

}

[[nodiscard]] ALWAYS_INLINE AK_UNICODE_CODE_POINT_LITERAL_CONSTEVAL AK::UnicodeCodePoint operator""_code_point(char32_t cp)
{
    return AK::UnicodeCodePoint(static_cast<u32>(cp));
}

#if USING_AK_GLOBALLY
using AK::UnicodeCodePoint;
#endif
