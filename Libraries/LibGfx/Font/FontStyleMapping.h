/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>

namespace Gfx {

struct FontStyleMapping {
    int style { 0 };
    StringView name;
};

static constexpr Array<FontStyleMapping, 4> font_slope_names = { {
    { 0, "Regular"sv },
    { 1, "Italic"sv },
    { 2, "Oblique"sv },
    { 3, "Reclined"sv },
} };

static constexpr int name_to_slope(StringView name)
{
    for (auto& it : font_slope_names) {
        if (it.name == name)
            return it.style;
    }
    return {};
}

}
