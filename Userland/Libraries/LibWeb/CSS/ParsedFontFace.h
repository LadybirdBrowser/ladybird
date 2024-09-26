/*
 * Copyright (c) 2022-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibGfx/Font/UnicodeRange.h>
#include <LibURL/URL.h>
#include <LibWeb/CSS/Percentage.h>

namespace Web::CSS {

class ParsedFontFace {
public:
    struct Source {
        Variant<String, URL::URL> local_or_url;
        // FIXME: Do we need to keep this around, or is it only needed to discard unwanted formats during parsing?
        Optional<FlyString> format;
    };

    ParsedFontFace(FlyString font_family, Optional<int> weight, Optional<int> slope, Vector<Source> sources, Vector<Gfx::UnicodeRange> unicode_ranges, Optional<Percentage> ascent_override, Optional<Percentage> descent_override, Optional<Percentage> line_gap_override);
    ~ParsedFontFace() = default;

    Optional<Percentage> ascent_override() const { return m_ascent_override; }
    Optional<Percentage> descent_override() const { return m_descent_override; }
    FlyString font_family() const { return m_font_family; }
    Optional<int> slope() const { return m_slope; }
    Optional<int> weight() const { return m_weight; }
    Optional<Percentage> line_gap_override() const { return m_line_gap_override; }
    Vector<Source> const& sources() const { return m_sources; }
    Vector<Gfx::UnicodeRange> const& unicode_ranges() const { return m_unicode_ranges; }

private:
    FlyString m_font_family;
    Optional<int> m_weight { 0 };
    Optional<int> m_slope { 0 };
    Vector<Source> m_sources;
    Vector<Gfx::UnicodeRange> m_unicode_ranges;
    Optional<Percentage> m_ascent_override;
    Optional<Percentage> m_descent_override;
    Optional<Percentage> m_line_gap_override;
};

}
