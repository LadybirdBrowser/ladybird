/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
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

class WEB_API FontPlugin {
public:
    AK_ALLOC_WITH_KMALLOC;

    FontPlugin(bool is_layout_test_mode, Gfx::SystemFontProvider* = nullptr);
    ~FontPlugin();

    static FontPlugin& the();
    static void install(FontPlugin&);

    RefPtr<Gfx::Font> default_font(float point_size, Optional<Gfx::FontVariationSettings> const& font_variation_settings = {}, Optional<Gfx::ShapeFeatures> const& shape_features = {});
    Gfx::Font& default_fixed_width_font();

    FlyString generic_font_name(GenericFont);
    Vector<FlyString> symbol_font_names();

    bool is_layout_test_mode() const { return m_is_layout_test_mode; }

    void set_system_font_family(FlyString);

    void update_generic_fonts();

private:
    FlyString compute_generic_font_name(GenericFont);

    Vector<Vector<FlyString>> m_generic_font_fallbacks;
    // Filled from wherever a font cascade is resolved, which need not be the document thread.
    Mutex m_generic_font_cache_mutex;
    HashMap<GenericFont, FlyString> m_generic_font_cache;
    Vector<FlyString> m_symbol_font_names;
    RefPtr<Gfx::Font> m_default_fixed_width_font;
    Optional<FlyString> m_system_font_family;
    bool m_is_layout_test_mode { false };
};

}
