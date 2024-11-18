/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/UnicodeCodePointView.h>
#include <AK/Utf8Mixin.h>
#include <AK/Wtf8View.h>

namespace AK {

class Utf8View : public UnicodeCodePointViewBase<Utf8View, char8_t, UnicodeCodePointIterableBase<Utf8View, Utf8View, Wtf8View>>
    , public Utf8Mixin<Utf8View> {
    friend class Utf8Mixin<Utf8View>;

public:
    using UnicodeCodePointViewBase::UnicodeCodePointViewBase;

    static constexpr bool is_lossy = false;

    Optional<UnicodeCodePoint> chomp_one_left() &
    {
        return Utf8Mixin<Utf8View>::chomp_one_utf8_codepoint_left<Utf8Mixin<Utf8View>::AllowSurrogates::No>();
    }

    Optional<UnicodeCodePoint> chomp_one_right() &
    {
        return Utf8Mixin<Utf8View>::chomp_one_utf8_codepoint_right<Utf8Mixin<Utf8View>::AllowSurrogates::No>();
    }
};

template<>
struct Formatter<Utf8View> : Formatter<Wtf8View> { };

}

// FIXME: Remove this when clang on BSD distributions fully support consteval (specifically in the context of default parameter initialization).
//        Note that this is fixed in clang-15, but is not yet picked up by all downstream distributions.
//        See: https://github.com/llvm/llvm-project/issues/48230
//        Additionally, oss-fuzz currently ships an llvm-project commit that is a pre-release of 15.0.0.
//        See: https://github.com/google/oss-fuzz/issues/9989
//        Android currently doesn't ship clang-15 in any NDK
#if defined(AK_OS_BSD_GENERIC) || defined(OSS_FUZZ) || defined(AK_OS_ANDROID)
#    define AK_ASCII_UTF8_VIEW_LITERAL_CONSTEVAL constexpr
#else
#    define AK_ASCII_UTF8_VIEW_LITERAL_CONSTEVAL consteval
#endif

[[nodiscard]] ALWAYS_INLINE AK_ASCII_UTF8_VIEW_LITERAL_CONSTEVAL AK::Utf8View operator""_utf8(char8_t const* cstring, size_t length)
{
    using AK::ReadonlySpan;
    using AK::Utf8Mixin;
    using AK::Utf8View;

    auto sv = ReadonlySpan<char8_t> { cstring, length };
    VERIFY(Utf8Mixin<Utf8View>::consteval_validate<Utf8Mixin<Utf8View>::AllowedCodePoints::UnicodeOnly>(sv));
    return Utf8View::from_span_unchecked(sv);
}
