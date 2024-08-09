/*
 * Copyright (c) 2020, Srimanta Barua <srimanta.barua1@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Endian.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/OpenType/Tables.h>
#include <LibGfx/Size.h>
#include <math.h>

namespace OpenType {

// https://learn.microsoft.com/en-us/typography/opentype/spec/loca
// loca: Index to Location
class Loca {
public:
    static ErrorOr<Loca> from_slice(ReadonlyBytes, u32 num_glyphs, IndexToLocFormat);
    u32 get_glyph_offset(u32 glyph_id) const;

private:
    Loca(ReadonlyBytes slice, u32 num_glyphs, IndexToLocFormat index_to_loc_format)
        : m_slice(slice)
        , m_num_glyphs(num_glyphs)
        , m_index_to_loc_format(index_to_loc_format)
    {
    }

    ReadonlyBytes m_slice;
    u32 m_num_glyphs { 0 };
    IndexToLocFormat m_index_to_loc_format;
};

// https://learn.microsoft.com/en-us/typography/opentype/spec/glyf
// glyf: Glyph Data
class Glyf {
public:
    class Glyph {
    public:
        Glyph(ReadonlyBytes slice, i16 xmin, i16 ymin, i16 xmax, i16 ymax, i16 num_contours = -1)
            : m_xmin(xmin)
            , m_ymin(ymin)
            , m_xmax(xmax)
            , m_ymax(ymax)
            , m_num_contours(num_contours)
            , m_slice(slice)
        {
            if (m_num_contours >= 0) {
                m_type = Type::Simple;
            }
        }

        i16 xmax() const { return m_xmax; }
        i16 xmin() const { return m_xmin; }

        int ascender() const { return m_ymax; }
        int descender() const { return m_ymin; }

    private:
        enum class Type {
            Simple,
            Composite,
        };

        Type m_type { Type::Composite };
        i16 m_xmin { 0 };
        i16 m_ymin { 0 };
        i16 m_xmax { 0 };
        i16 m_ymax { 0 };
        i16 m_num_contours { -1 };
        ReadonlyBytes m_slice;
    };

    Glyf(ReadonlyBytes slice)
        : m_slice(slice)
    {
    }
    Optional<Glyph> glyph(u32 offset) const;

private:
    // https://learn.microsoft.com/en-us/typography/opentype/spec/glyf#glyph-headers
    struct [[gnu::packed]] GlyphHeader {
        BigEndian<i16> number_of_contours;
        BigEndian<i16> x_min;
        BigEndian<i16> y_min;
        BigEndian<i16> x_max;
        BigEndian<i16> y_max;
    };
    static_assert(AssertSize<GlyphHeader, 10>());

    ReadonlyBytes m_slice;
};

}
