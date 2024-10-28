/*
 * Copyright (c) 2020, Stephan Unverwerth <s.unverwerth@serenityos.org>
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
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

struct hb_font_t;

namespace Gfx {

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

constexpr float text_shaping_resolution = 64;

class Typeface;

class Font : public RefCounted<Font> {
public:
    virtual ~Font();

    virtual FontPixelMetrics pixel_metrics() const = 0;

    virtual u8 slope() const = 0;

    // Font point size (distance between ascender and descender).
    virtual float point_size() const = 0;

    // Font pixel size (distance between ascender and descender).
    virtual float pixel_size() const = 0;

    // Font pixel size, rounded up to the nearest integer.
    virtual int pixel_size_rounded_up() const = 0;

    virtual u16 weight() const = 0;
    virtual bool contains_glyph(u32 code_point) const = 0;

    virtual u32 glyph_id_for_code_point(u32 code_point) const = 0;
    virtual float glyph_width(u32 code_point) const = 0;
    virtual int x_height() const = 0;
    virtual float preferred_line_height() const = 0;

    virtual u8 baseline() const = 0;

    virtual float width(StringView) const = 0;
    virtual float width(Utf8View const&) const = 0;

    virtual FlyString family() const = 0;

    virtual NonnullRefPtr<Font> with_size(float point_size) const = 0;

    Font const& bold_variant() const;
    hb_font_t* harfbuzz_font() const;

    virtual Typeface const& typeface() const = 0;

private:
    mutable RefPtr<Gfx::Font const> m_bold_variant;
    mutable hb_font_t* m_harfbuzz_font { nullptr };
};

}
