/*
 * Copyright (c) 2020, Srimanta Barua <srimanta.barua1@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Endian.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/OpenType/Tables.h>
#include <LibGfx/Path.h>
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
    enum class CompositeFlags {
        Arg1AndArg2AreWords = 0x0001,
        ArgsAreXYValues = 0x0002,
        RoundXYToGrid = 0x0004,
        WeHaveAScale = 0x0008,
        MoreComponents = 0x0020,
        WeHaveAnXAndYScale = 0x0040,
        WeHaveATwoByTwo = 0x0080,
        WeHaveInstructions = 0x0100,
        UseMyMetrics = 0x0200,
        OverlapCompound = 0x0400, // Not relevant - can overlap without this set
        ScaledComponentOffset = 0x0800,
        UnscaledComponentOffset = 0x1000,
    };

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

        template<typename GlyphCb>
        bool append_path(Gfx::Path& path, i16 font_ascender, i16 font_descender, float x_scale, float y_scale, GlyphCb glyph_callback) const
        {
            switch (m_type) {
            case Type::Simple:
                return append_simple_path(path, font_ascender, font_descender, x_scale, y_scale);
            case Type::Composite:
                return append_composite_path(path, font_ascender, x_scale, y_scale, glyph_callback);
            }
            VERIFY_NOT_REACHED();
        }

        i16 xmax() const { return m_xmax; }
        i16 xmin() const { return m_xmin; }

        int ascender() const { return m_ymax; }
        int descender() const { return m_ymin; }

        ReadonlyBytes program() const;

    private:
        enum class Type {
            Simple,
            Composite,
        };

        class ComponentIterator {
        public:
            struct Item {
                u16 glyph_id;
                Gfx::AffineTransform affine;
            };

            ComponentIterator(ReadonlyBytes slice)
                : m_slice(slice)
            {
            }
            Optional<Item> next();

        private:
            ReadonlyBytes m_slice;
            bool m_has_more { true };
            u32 m_offset { 0 };
        };

        void append_path_impl(Gfx::Path&, Gfx::AffineTransform const&) const;
        bool append_simple_path(Gfx::Path&, i16 ascender, i16 descender, float x_scale, float y_scale) const;

        template<typename GlyphCb>
        void resolve_composite_path_loop(Gfx::Path& path, Gfx::AffineTransform const& transform, GlyphCb glyph_callback) const
        {
            ComponentIterator component_iterator(m_slice);

            while (true) {
                auto opt_item = component_iterator.next();
                if (!opt_item.has_value()) {
                    break;
                }
                auto item = opt_item.value();
                Gfx::AffineTransform affine_here { transform };
                affine_here.multiply(item.affine);
                auto glyph = glyph_callback(item.glyph_id);
                if (!glyph.has_value())
                    continue;

                if (glyph->m_type == Type::Simple) {
                    glyph->append_path_impl(path, affine_here);
                } else {
                    glyph->resolve_composite_path_loop(path, transform, glyph_callback);
                }
            }
        }

        template<typename GlyphCb>
        bool append_composite_path(Gfx::Path& path, i16 font_ascender, float x_scale, float y_scale, GlyphCb glyph_callback) const
        {
            auto affine = Gfx::AffineTransform()
                              .translate(path.last_point())
                              .scale(x_scale, -y_scale)
                              .translate(-m_xmin, -font_ascender);
            resolve_composite_path_loop(path, affine, glyph_callback);
            return true;
        }

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
