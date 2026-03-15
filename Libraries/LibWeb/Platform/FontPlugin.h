/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/HashMap.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/FontVariationSettings.h>
#include <LibGfx/ShapeFeature.h>
#include <LibWeb/Export.h>

namespace Web::Platform {

enum class GenericFont {
    Cursive,
    Fantasy,
    Monospace,
    SansSerif,
    Serif,
    UiMonospace,
    UiRounded,
    UiSansSerif,
    UiSerif,
    __Count,
};

struct GenericFontKey {
    GenericFont generic_font;
    int weight;
    int slope;

    bool operator==(GenericFontKey const&) const = default;
};

class WEB_API FontPlugin {
public:
    FontPlugin(bool is_layout_test_mode, Gfx::SystemFontProvider* = nullptr);
    ~FontPlugin();

    static FontPlugin& the();
    static void install(FontPlugin&);

    RefPtr<Gfx::Font> default_font(float point_size, Optional<Gfx::FontVariationSettings> const& font_variation_settings = {}, Optional<Gfx::ShapeFeatures> const& shape_features = {});
    Gfx::Font& default_fixed_width_font();

    FlyString generic_font_name(GenericFont, int weight, int slope);
    Vector<FlyString> symbol_font_names();

    bool is_layout_test_mode() const { return m_is_layout_test_mode; }

    void update_generic_fonts();

private:
    FlyString compute_generic_font_name(GenericFont, int weight, int slope);

    Vector<Vector<FlyString>> m_generic_font_fallbacks;
    HashMap<GenericFontKey, FlyString> m_generic_font_cache;
    Vector<FlyString> m_symbol_font_names;
    RefPtr<Gfx::Font> m_default_fixed_width_font;
    bool m_is_layout_test_mode { false };
};

}

template<>
struct AK::Traits<Web::Platform::GenericFontKey> : public AK::DefaultTraits<Web::Platform::GenericFontKey> {
    static unsigned hash(Web::Platform::GenericFontKey const& key)
    {
        return pair_int_hash(pair_int_hash(to_underlying(key.generic_font), key.weight), key.slope);
    }
};
