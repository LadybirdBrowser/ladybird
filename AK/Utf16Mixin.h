/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Debug.h>
#include <AK/Forward.h>
#include <AK/UnicodeCodePointView.h>

namespace AK {

// https://www.unicode.org/versions/Unicode16.0.0/UnicodeStandard-16.0.pdf#page=167
template<typename Self>
class Utf16Mixin {
public:
    enum class AllowedCodePoints {
        UnicodeOnly,          // U+0000..U+D800 & U+E000..U+10FFFF
        UnicodeAndSurrogates, // U+0000..U+10FFFF
    };

    template<AllowedCodePoints allowed_code_points>
    Optional<u32> chomp_one_utf16_codepoint_left()
    {
        TODO();
    }

    template<AllowedCodePoints allowed_code_points>
    Optional<u32> chomp_one_utf8_codepoint_right()
    {
        TODO();
    }
};

}
