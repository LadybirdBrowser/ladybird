/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/UnicodeCodePointView.h>
#include <AK/Utf8Mixin.h>

namespace AK {

class Wtf8View : public UnicodeCodePointViewBase<Wtf8View, char8_t>
    , public Utf8Mixin<Wtf8View> {
    AK_MAKE_DEFAULT_COPYABLE(Wtf8View);
    AK_MAKE_DEFAULT_MOVABLE(Wtf8View);

    friend class Utf8Mixin;

public:
    using UnicodeCodePointViewBase::UnicodeCodePointViewBase;

    static constexpr bool is_lossy = false;

    Optional<UnicodeCodePoint> chomp_one_left() &
    {
        return Utf8Mixin::chomp_one_utf8_codepoint_left<AllowSurrogates::Yes>();
    }

    Optional<UnicodeCodePoint> chomp_one_right() &
    {
        return Utf8Mixin::chomp_one_utf8_codepoint_right<AllowSurrogates::Yes>();
    }

    Utf8View validated() const;
};

template<>
struct Formatter<Wtf8View> : Formatter<char8_t> {
    ErrorOr<void> format(FormatBuilder& builder, Wtf8View const& view)
    {
        for (auto cp : view)
            TRY(builder.put_code_point(cp));

        return {};
    }
};

}

// FIXME: Remove this when clang on BSD distributions fully support consteval (specifically in the context of default parameter initialization).
//        Note that this is fixed in clang-15, but is not yet picked up by all downstream distributions.
//        See: https://github.com/llvm/llvm-project/issues/48230
//        Additionally, oss-fuzz currently ships an llvm-project commit that is a pre-release of 15.0.0.
//        See: https://github.com/google/oss-fuzz/issues/9989
//        Android currently doesn't ship clang-15 in any NDK
#if defined(AK_OS_BSD_GENERIC) || defined(OSS_FUZZ) || defined(AK_OS_ANDROID)
#    define AK_ASCII_WTF8_VIEW_LITERAL_CONSTEVAL constexpr
#else
#    define AK_ASCII_WTF8_VIEW_LITERAL_CONSTEVAL consteval
#endif

[[nodiscard]] ALWAYS_INLINE AK_ASCII_WTF8_VIEW_LITERAL_CONSTEVAL AK::Wtf8View operator""_wtf8(char8_t const* cstring, size_t length)
{
    using AK::ReadonlySpan;
    using AK::Utf8Mixin;
    using AK::Wtf8View;

    auto sv = ReadonlySpan<char8_t> { cstring, length };
    VERIFY(Utf8Mixin<Wtf8View>::consteval_validate<Utf8Mixin<Wtf8View>::AllowedCodePoints::UnicodeAndSurrogates>(sv));
    return Wtf8View::from_span_unchecked(sv);
}
