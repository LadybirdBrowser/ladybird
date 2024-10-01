/*
 * Copyright (c) 2022-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/HashMap.h>
#include <LibGfx/Font/UnicodeRange.h>
#include <LibURL/URL.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/Percentage.h>

namespace Web::CSS {

class ParsedFontFace {
public:
    struct Source {
        Variant<String, URL::URL> local_or_url;
        // FIXME: Do we need to keep this around, or is it only needed to discard unwanted formats during parsing?
        Optional<FlyString> format;
    };

    ParsedFontFace(FlyString font_family, Optional<int> weight, Optional<int> slope, Optional<int> width, Vector<Source> sources, Vector<Gfx::UnicodeRange> unicode_ranges, Optional<Percentage> ascent_override, Optional<Percentage> descent_override, Optional<Percentage> line_gap_override, FontDisplay font_display, Optional<FlyString> font_named_instance, Optional<FlyString> font_language_override, Optional<OrderedHashMap<FlyString, i64>> font_feature_settings, Optional<OrderedHashMap<FlyString, double>> font_variation_settings);
    ~ParsedFontFace() = default;

    Optional<Percentage> ascent_override() const { return m_ascent_override; }
    Optional<Percentage> descent_override() const { return m_descent_override; }
    FontDisplay font_display() const { return m_font_display; }
    FlyString font_family() const { return m_font_family; }
    Optional<OrderedHashMap<FlyString, i64>> font_feature_settings() const { return m_font_feature_settings; }
    Optional<FlyString> font_language_override() const { return m_font_language_override; }
    Optional<FlyString> font_named_instance() const { return m_font_named_instance; }
    Optional<OrderedHashMap<FlyString, double>> font_variation_settings() const { return m_font_variation_settings; }
    Optional<int> slope() const { return m_slope; }
    Optional<int> weight() const { return m_weight; }
    Optional<int> width() const { return m_width; }
    Optional<Percentage> line_gap_override() const { return m_line_gap_override; }
    Vector<Source> const& sources() const { return m_sources; }
    Vector<Gfx::UnicodeRange> const& unicode_ranges() const { return m_unicode_ranges; }

private:
    FlyString m_font_family;
    Optional<FlyString> m_font_named_instance;
    Optional<int> m_weight;
    Optional<int> m_slope;
    Optional<int> m_width;
    Vector<Source> m_sources;
    Vector<Gfx::UnicodeRange> m_unicode_ranges;
    Optional<Percentage> m_ascent_override;
    Optional<Percentage> m_descent_override;
    Optional<Percentage> m_line_gap_override;
    FontDisplay m_font_display;
    Optional<FlyString> m_font_language_override;
    Optional<OrderedHashMap<FlyString, i64>> m_font_feature_settings;
    Optional<OrderedHashMap<FlyString, double>> m_font_variation_settings;
};

}
