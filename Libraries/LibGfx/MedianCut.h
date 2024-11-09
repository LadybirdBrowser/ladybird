/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>

namespace Gfx {

class ColorPalette {
public:
    struct ColorAndIndex {
        Color color;
        size_t index;
    };

    ColorPalette(Vector<Color> palette, HashMap<Color, ColorAndIndex> conversion_table)
        : m_palette(move(palette))
        , m_conversion_table(move(conversion_table))
    {
    }

    Vector<Color> const& palette() const
    {
        return m_palette;
    }

    Color closest_color(Color input) const
    {
        return m_palette[index_of_closest_color(input)];
    }

    u32 index_of_closest_color(Color input) const
    {
        if (auto const result = m_conversion_table.get(input); result.has_value())
            return result->index;
        TODO();
    }

private:
    Vector<Color> m_palette;
    HashMap<Color, ColorAndIndex> m_conversion_table;
};

ErrorOr<ColorPalette> median_cut(Bitmap const& bitmap, u16 palette_size);

}
