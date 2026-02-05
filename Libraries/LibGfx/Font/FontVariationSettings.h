/*
 * Copyright (c) 2025, Norbiros <me@norbiros.dev>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/QuickSort.h>
#include <AK/Vector.h>
#include <LibGfx/FourCC.h>

#pragma once

namespace Gfx {

struct FontVariationAxis {
    FourCC tag;
    float value;

    FontVariationAxis(FourCC t, float v)
        : tag(t)
        , value(v)
    {
    }

    bool operator==(FontVariationAxis const& other) const
    {
        return tag == other.tag && value == other.value;
    }
};

// FIXME: Support other named axes like 'slnt', 'ital', 'opsz', 'GRAD', etc.
struct FontVariationSettings {
    HashMap<FourCC, float> axes;

    FontVariationSettings() = default;

    // https://learn.microsoft.com/en-us/typography/opentype/spec/dvaraxistag_wght
    void set_weight(float value)
    {
        axes.set(FourCC("wght"), value);
    }

    // https://learn.microsoft.com/en-us/typography/opentype/spec/dvaraxistag_wdth
    void set_width(float value)
    {
        axes.set(FourCC("wdth"), value);
    }

    // https://learn.microsoft.com/en-us/typography/opentype/spec/dvaraxistag_opsz
    void set_optical_sizing(float value)
    {
        axes.set(FourCC("opsz"), value);
    }

    bool is_empty() const { return axes.is_empty(); }

    Vector<FontVariationAxis> to_sorted_list() const
    {
        Vector<FontVariationAxis> list;
        list.ensure_capacity(axes.size());

        for (auto const& entry : axes)
            list.unchecked_append(FontVariationAxis(entry.key, entry.value));

        quick_sort(list, [](auto const& a, auto const& b) {
            return a.tag < b.tag;
        });

        return list;
    }
};

}
