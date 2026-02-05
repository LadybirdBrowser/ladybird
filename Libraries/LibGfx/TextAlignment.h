/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/StringView.h>

namespace Gfx {

#define GFX_ENUMERATE_TEXT_ALIGNMENTS(M) \
    M(Center)                            \
    M(CenterLeft)                        \
    M(CenterRight)                       \
    M(TopCenter)                         \
    M(TopLeft)                           \
    M(TopRight)                          \
    M(BottomCenter)                      \
    M(BottomLeft)                        \
    M(BottomRight)

enum class TextAlignment {
#define __ENUMERATE(x) x,
    GFX_ENUMERATE_TEXT_ALIGNMENTS(__ENUMERATE)
#undef __ENUMERATE
};

inline bool is_right_text_alignment(TextAlignment alignment)
{
    switch (alignment) {
    case TextAlignment::CenterRight:
    case TextAlignment::TopRight:
    case TextAlignment::BottomRight:
        return true;
    default:
        return false;
    }
}

inline bool is_vertically_centered_text_alignment(TextAlignment alignment)
{
    switch (alignment) {
    case TextAlignment::CenterLeft:
    case TextAlignment::CenterRight:
    case TextAlignment::Center:
        return true;
    default:
        return false;
    }
}

inline Optional<TextAlignment> text_alignment_from_string(StringView string)
{
#define __ENUMERATE(x) \
    if (string == #x)  \
        return TextAlignment::x;
    GFX_ENUMERATE_TEXT_ALIGNMENTS(__ENUMERATE)
#undef __ENUMERATE
    return {};
}

inline char const* to_string(TextAlignment text_alignment)
{
#define __ENUMERATE(x)                      \
    if (text_alignment == TextAlignment::x) \
        return #x;
    GFX_ENUMERATE_TEXT_ALIGNMENTS(__ENUMERATE)
#undef __ENUMERATE
    return {};
}

}
