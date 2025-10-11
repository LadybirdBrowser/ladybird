/*
 * Copyright (c) 2025, Norbiros <me@norbiros.dev>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/QuickSort.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/CalculatedOr.h>

#pragma once

namespace Gfx {

using FourByteTag = uint32_t;

constexpr FourByteTag MakeFourByteTag(char a, char b, char c, char d)
{
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(c) << 8) | static_cast<uint32_t>(d);
}

struct FontVariationAxis {
    FourByteTag tag;
    float value;

    FontVariationAxis(FourByteTag t, float v)
        : tag(t)
        , value(v)
    {
    }

    bool operator==(FontVariationAxis const& other) const
    {
        return tag == other.tag && value == other.value;
    }
};

struct FontVariationSettings {
    HashMap<FourByteTag, float> axes;

    FontVariationSettings() = default;

    void weight(float w)
    {
        axes.set(MakeFourByteTag('w', 'g', 'h', 't'), w);
    }

    void width(float w)
    {
        axes.set(MakeFourByteTag('w', 'd', 't', 'h'), w);
    }

    bool is_empty() const { return axes.is_empty(); }

    Vector<FontVariationAxis> to_sorted_list() const
    {
        Vector<FontVariationAxis> list;
        for (auto const& entry : axes)
            list.append(FontVariationAxis(entry.key, entry.value));

        quick_sort(list, [](auto const& a, auto const& b) {
            return a.tag < b.tag;
        });

        return list;
    }

    void update(HashMap<FlyString, Web::CSS::NumberOrCalculated> const& input)
    {

        for (auto const& [tag_string, value] : input) {
            auto string_view = tag_string.bytes_as_string_view();

            if (string_view.length() != 4)
                continue;

            auto tag = Gfx::MakeFourByteTag(
                string_view[0],
                string_view[1],
                string_view[2],
                string_view[3]);

            axes.set(tag, value.value());
        }
    }
};

}
