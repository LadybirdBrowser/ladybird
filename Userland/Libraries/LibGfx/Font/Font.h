/*
 * Copyright (c) 2020, Stephan Unverwerth <s.unverwerth@serenityos.org>
 * Copyright (c) 2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Bitmap.h>
#include <AK/ByteReader.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <LibCore/MappedFile.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Size.h>

namespace Gfx {

class Glyph {
public:
    Glyph(NonnullRefPtr<Bitmap> bitmap, float left_bearing, float advance, float ascent, bool is_color_bitmap)
        : m_bitmap(bitmap)
        , m_left_bearing(left_bearing)
        , m_advance(advance)
        , m_ascent(ascent)
        , m_color_bitmap(is_color_bitmap)
    {
    }

    bool is_color_bitmap() const { return m_color_bitmap; }

    [[nodiscard]] NonnullRefPtr<Bitmap> bitmap() const { return m_bitmap; }
    float left_bearing() const { return m_left_bearing; }
    float advance() const { return m_advance; }
    float ascent() const { return m_ascent; }

private:
    NonnullRefPtr<Bitmap> m_bitmap;
    float m_left_bearing;
    float m_advance;
    float m_ascent;
    bool m_color_bitmap { false };
};

struct GlyphSubpixelOffset {
    u8 x;
    u8 y;

    // TODO: Allow setting this at runtime via some config?
    static constexpr int subpixel_divisions() { return 3; }
    FloatPoint to_float_point() const { return FloatPoint(x / float(subpixel_divisions()), y / float(subpixel_divisions())); }

    bool operator==(GlyphSubpixelOffset const&) const = default;
};

struct GlyphRasterPosition {
    // Where the glyph bitmap should be drawn/blitted.
    IntPoint blit_position;

    // A subpixel offset to be used when rendering the glyph.
    // This improves kerning and alignment at the expense of caching a few extra bitmaps.
    // This is (currently) snapped to thirds of a subpixel (i.e. 0, 0.33, 0.66).
    GlyphSubpixelOffset subpixel_offset;

    static GlyphRasterPosition get_nearest_fit_for(FloatPoint position);
};

struct FontPixelMetrics {
    float size { 0 };
    float x_height { 0 };
    float advance_of_ascii_zero { 0 };

    // Number of pixels the font extends above the baseline.
    float ascent { 0 };

    // Number of pixels the font descends below the baseline.
    float descent { 0 };

    // Line gap specified by font.
    float line_gap { 0 };

    float line_spacing() const { return ascent + descent + line_gap; }
};

// https://learn.microsoft.com/en-us/typography/opentype/spec/os2#uswidthclass
enum FontWidth {
    UltraCondensed = 1,
    ExtraCondensed = 2,
    Condensed = 3,
    SemiCondensed = 4,
    Normal = 5,
    SemiExpanded = 6,
    Expanded = 7,
    ExtraExpanded = 8,
    UltraExpanded = 9
};

class Typeface;

class Font : public RefCounted<Font> {
public:
    virtual ~Font() {};

    virtual FontPixelMetrics pixel_metrics() const = 0;

    virtual u8 slope() const = 0;

    // Font point size (distance between ascender and descender).
    virtual float point_size() const = 0;

    // Font pixel size (distance between ascender and descender).
    virtual float pixel_size() const = 0;

    // Font pixel size, rounded up to the nearest integer.
    virtual int pixel_size_rounded_up() const = 0;

    virtual u16 weight() const = 0;
    virtual Optional<Glyph> glyph(u32 code_point) const = 0;
    virtual Optional<Glyph> glyph(u32 code_point, GlyphSubpixelOffset) const = 0;
    virtual bool contains_glyph(u32 code_point) const = 0;

    virtual bool append_glyph_path_to(Gfx::Path&, u32 glyph_id) const = 0;
    virtual u32 glyph_id_for_code_point(u32 code_point) const = 0;
    virtual float glyph_left_bearing(u32 code_point) const = 0;
    virtual float glyph_width(u32 code_point) const = 0;
    virtual float glyph_or_emoji_width(Utf8CodePointIterator&) const = 0;
    virtual float glyphs_horizontal_kerning(u32 left_code_point, u32 right_code_point) const = 0;
    virtual int x_height() const = 0;
    virtual float preferred_line_height() const = 0;

    virtual u8 baseline() const = 0;

    virtual float width(StringView) const = 0;
    virtual float width(Utf8View const&) const = 0;

    virtual String family() const = 0;
    virtual String variant() const = 0;

    virtual NonnullRefPtr<Font> with_size(float point_size) const = 0;

    Font const& bold_variant() const;

    virtual bool has_color_bitmaps() const = 0;

    virtual Typeface const& typeface() const = 0;

private:
    mutable RefPtr<Gfx::Font const> m_bold_variant;
};

}
