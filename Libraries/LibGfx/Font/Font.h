/*
 * Copyright (c) 2020, Stephan Unverwerth <s.unverwerth@serenityos.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2023-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Utf16String.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/Typeface.h>
#include <LibGfx/ShapeFeature.h>

class SkFont;
struct hb_font_t;
struct hb_buffer_t;

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

class Font : public RefCounted<Font> {
public:
    Font(NonnullRefPtr<Typeface const>, float point_width, float point_height, unsigned dpi_x, unsigned dpi_y, FontVariationSettings const variations, ShapeFeatures const& features);
    ScaledFontMetrics metrics() const;
    ~Font();

    float point_size() const;
    float pixel_size() const;
    FontPixelMetrics const& pixel_metrics() const { return m_pixel_metrics; }
    u8 slope() const { return m_typeface->slope(); }
    u16 weight() const { return m_typeface->weight(); }
    bool contains_glyph(u32 code_point) const { return m_typeface->glyph_id_for_code_point(code_point) > 0; }
    float glyph_width(u32 code_point) const;
    u32 glyph_id_for_code_point(u32 code_point) const { return m_typeface->glyph_id_for_code_point(code_point); }
    float preferred_line_height() const { return metrics().height() + metrics().line_gap; }
    int x_height() const { return m_point_height; } // FIXME: Read from font
    u8 baseline() const { return m_point_height; }  // FIXME: Read from font
    float width(Utf16View const&) const;
    FlyString const& family() const { return m_typeface->family(); }

    NonnullRefPtr<Font> scaled_with_size(float point_size) const;
    NonnullRefPtr<Font> with_size(float point_size) const;

    Typeface const& typeface() const { return m_typeface; }

    SkFont skia_font(float scale) const;

    Font const& bold_variant() const;
    hb_font_t* harfbuzz_font() const;
    ShapeFeatures const& features() const { return m_shape_features; }

    struct ShapingCache {
        HashMap<Utf16String, hb_buffer_t*> map;
        hb_buffer_t* single_ascii_character_map[128] { nullptr };

        ~ShapingCache();
        void clear();
    };
    ShapingCache& shaping_cache() const { return m_shaping_cache; }

    bool is_emoji_font() const;

private:
    mutable RefPtr<Font const> m_bold_variant;
    mutable hb_font_t* m_harfbuzz_font { nullptr };

    mutable ShapingCache m_shaping_cache;

    mutable TriState m_is_emoji_font { TriState::Unknown };

    NonnullRefPtr<Typeface const> m_typeface;
    float m_x_scale { 0.0f };
    float m_y_scale { 0.0f };
    float m_point_width { 0.0f };
    float m_point_height { 0.0f };
    FontVariationSettings const m_font_variation_settings;
    ShapeFeatures m_shape_features;
    FontPixelMetrics m_pixel_metrics;

    float m_pixel_size { 0.0f };
};

}
