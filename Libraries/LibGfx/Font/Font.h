/*
 * Copyright (c) 2020, Stephan Unverwerth <s.unverwerth@serenityos.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2023-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/FlyString.h>
#include <AK/Once.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Utf16String.h>
#include <LibGfx/Font/RasterizerData.h>
#include <LibGfx/Font/Typeface.h>
#include <LibGfx/ShapeFeature.h>

struct hb_font_t;
struct hb_buffer_t;

namespace Gfx {

struct FontPixelMetrics {
    float x_height { 0 };
    float advance_of_ascii_zero { 0 };

    // Number of pixels the font extends above the baseline.
    float ascent { 0 };

    // Number of pixels the font descends below the baseline.
    float descent { 0 };
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

class Font : public AtomicRefCounted<Font> {
public:
    Font(NonnullRefPtr<Typeface const>, float point_width, float point_height, FontVariationSettings const variations, ShapeFeatures const& features);
    ~Font();

    u64 id() const { return m_id; }
    float point_size() const;
    float pixel_size() const;
    FontPixelMetrics const& pixel_metrics() const { return m_pixel_metrics; }
    // Metrics that only font-size-adjust reads, measured when asked for rather than with the pixel metrics.
    float cap_height() const;
    Optional<float> horizontal_advance_of_code_point(u32 code_point) const;
    Optional<float> vertical_advance_of_code_point(u32 code_point) const;
    u8 slope() const { return m_style.slope; }
    u16 weight() const { return m_style.weight; }
    u16 width() const { return m_style.width; }
    bool contains_glyph(u32 code_point) const { return m_typeface->glyph_id_for_code_point(code_point) > 0; }
    u32 glyph_id_for_code_point(u32 code_point) const { return m_typeface->glyph_id_for_code_point(code_point); }
    int x_height() const { return m_point_height; } // FIXME: Read from font
    float width(Utf16View const&) const;
    FlyString const& family() const { return m_typeface->family(); }

    NonnullRefPtr<Font> invisible_variant() const;
    bool is_invisible() const { return m_is_invisible; }

    Typeface const& typeface() const { return m_typeface; }

    hb_font_t* harfbuzz_font() const { return m_harfbuzz_font; }
    FontVariationSettings const& variation_settings() const { return m_font_variation_settings; }
    ShapeFeatures const& features() const { return m_shape_features; }

    bool is_emoji_font() const;

    // The constants of the OpenType MATH table and the underline thickness of the post table, read on first use.
    struct MathData;
    MathData const& math_data() const;

    // Called by AtomicRefCounted when the last reference goes away.
    void will_be_destroyed() const;

    // What a rasterizer keeps with this font. The first call makes it.
    template<typename T, typename Callback>
    T& rasterizer_data(Callback make) const { return m_rasterizer_data.get<T>(make); }

private:
    friend class Typeface;

    bool m_is_invisible { false };
    u64 m_id { 0 };

    hb_font_t* create_harfbuzz_font() const;
    FontPixelMetrics compute_pixel_metrics() const;

    hb_font_t* m_harfbuzz_font { nullptr };

    RasterizerDataSlot m_rasterizer_data;

    // A layout pass classifies fonts while the document thread may be doing the same to the same
    // font, so the verdict is a single atomic byte. Either winner is correct: the classification
    // reads only the face's immutable tables.
    mutable Atomic<TriState> m_is_emoji_font { TriState::Unknown };

    mutable OnceFlag m_math_data_once;
    mutable OwnPtr<MathData> m_math_data;

    NonnullRefPtr<Typeface const> m_typeface;
    float m_point_width { 0.0f };
    float m_point_height { 0.0f };
    FontVariationSettings const m_font_variation_settings;
    ShapeFeatures m_shape_features;
    FontPixelMetrics m_pixel_metrics;
    FaceStyle m_style;

    float m_pixel_size { 0.0f };
};

}
