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
    { 100, "Thin"sv },
    { 200, "Extra Light"sv },
    { 300, "Light"sv },
    { 400, "Regular"sv },
    { 500, "Medium"sv },
    { 600, "Semi Bold"sv },
    { 700, "Bold"sv },
    { 800, "Extra Bold"sv },
    { 900, "Black"sv },
    { 950, "Extra Black"sv },
} };

static constexpr Array<FontStyleMapping, 4> font_slope_names = { {
    { 0, "Regular"sv },
    { 1, "Italic"sv },
    { 2, "Oblique"sv },
    { 3, "Reclined"sv },
} };

static constexpr Array<FontStyleMapping, 9> font_width_names = { {
    { 1, "Ultra Condensed"sv },
    { 2, "Extra Condensed"sv },
    { 3, "Condensed"sv },
    { 4, "Semi Condensed"sv },
    { 5, "Normal"sv },
    { 6, "Semi Expanded"sv },
    { 7, "Expanded"sv },
    { 8, "Extra Expanded"sv },
    { 9, "Ultra Expanded"sv },
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
