/*
 * Copyright (c) 2022-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ParsedFontFace.h>

namespace Web::CSS {

ParsedFontFace::ParsedFontFace(FlyString font_family, Optional<int> weight, Optional<int> slope, Vector<Source> sources, Vector<Gfx::UnicodeRange> unicode_ranges, Optional<Percentage> ascent_override, Optional<Percentage> descent_override, Optional<Percentage> line_gap_override, FontDisplay font_display)
    : m_font_family(move(font_family))
    , m_weight(weight)
    , m_slope(slope)
    , m_sources(move(sources))
    , m_unicode_ranges(move(unicode_ranges))
    , m_ascent_override(move(ascent_override))
    , m_descent_override(move(descent_override))
    , m_line_gap_override(move(line_gap_override))
    , m_font_display(font_display)
{
}

}
