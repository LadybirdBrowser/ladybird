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
    M(CenterRight)

enum class TextAlignment {
#define __ENUMERATE(x) x,
    GFX_ENUMERATE_TEXT_ALIGNMENTS(__ENUMERATE)
#undef __ENUMERATE
};

}
