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

static constexpr Array<FontStyleMapping, 10> font_weight_names = { {
    { 100, "Thin"_sv },
    { 200, "Extra Light"_sv },
    { 300, "Light"_sv },
    { 400, "Regular"_sv },
    { 500, "Medium"_sv },
    { 600, "Semi Bold"_sv },
    { 700, "Bold"_sv },
    { 800, "Extra Bold"_sv },
    { 900, "Black"_sv },
    { 950, "Extra Black"_sv },
} };

static constexpr Array<FontStyleMapping, 4> font_slope_names = { {
    { 0, "Regular"_sv },
    { 1, "Italic"_sv },
    { 2, "Oblique"_sv },
    { 3, "Reclined"_sv },
} };

static constexpr Array<FontStyleMapping, 9> font_width_names = { {
    { 1, "Ultra Condensed"_sv },
    { 2, "Extra Condensed"_sv },
    { 3, "Condensed"_sv },
    { 4, "Semi Condensed"_sv },
    { 5, "Normal"_sv },
    { 6, "Semi Expanded"_sv },
    { 7, "Expanded"_sv },
    { 8, "Extra Expanded"_sv },
    { 9, "Ultra Expanded"_sv },
} };

static constexpr StringView weight_to_name(int weight)
{
    for (auto& it : font_weight_names) {
        if (it.style == weight)
            return it.name;
    }
    return {};
}

static constexpr int name_to_weight(StringView name)
{
    for (auto& it : font_weight_names) {
        if (it.name == name)
            return it.style;
    }
    return {};
}

static constexpr StringView slope_to_name(int slope)
{
    for (auto& it : font_slope_names) {
        if (it.style == slope)
            return it.name;
    }
    return {};
}

static constexpr int name_to_slope(StringView name)
{
    for (auto& it : font_slope_names) {
        if (it.name == name)
            return it.style;
    }
    return {};
}

static constexpr StringView width_to_name(int width)
{
    for (auto& it : font_width_names) {
        if (it.style == width)
            return it.name;
    }
    return {};
}

static constexpr int name_to_width(StringView name)
{
    for (auto& it : font_width_names) {
        if (it.name == name)
            return it.style;
    }
    return {};
}

}
