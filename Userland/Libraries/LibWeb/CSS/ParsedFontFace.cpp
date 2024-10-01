/*
 * Copyright (c) 2022-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ParsedFontFace.h>

namespace Web::CSS {

ParsedFontFace::ParsedFontFace(FlyString font_family, Optional<int> weight, Optional<int> slope, Optional<int> width, Vector<Source> sources, Vector<Gfx::UnicodeRange> unicode_ranges, Optional<Percentage> ascent_override, Optional<Percentage> descent_override, Optional<Percentage> line_gap_override, FontDisplay font_display, Optional<FlyString> font_named_instance, Optional<FlyString> font_language_override, Optional<OrderedHashMap<FlyString, i64>> font_feature_settings, Optional<OrderedHashMap<FlyString, double>> font_variation_settings)
    : m_font_family(move(font_family))
    , m_font_named_instance(move(font_named_instance))
    , m_weight(weight)
    , m_slope(slope)
    , m_width(width)
    , m_sources(move(sources))
    , m_unicode_ranges(move(unicode_ranges))
    , m_ascent_override(move(ascent_override))
    , m_descent_override(move(descent_override))
    , m_line_gap_override(move(line_gap_override))
    , m_font_display(font_display)
    , m_font_language_override(font_language_override)
    , m_font_feature_settings(move(font_feature_settings))
    , m_font_variation_settings(move(font_variation_settings))
{
}

}
