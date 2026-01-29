/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWebView/Forward.h>

namespace WebView {

class WEBVIEW_API FontPlugin final : public Web::Platform::FontPlugin {
public:
    FontPlugin(bool is_layout_test_mode, Gfx::SystemFontProvider* = nullptr);
    virtual ~FontPlugin();

    virtual RefPtr<Gfx::Font> default_font(float point_size, Optional<Gfx::FontVariationSettings> const& font_variation_settings = {}, Optional<Gfx::ShapeFeatures> const& shape_features = {}) override;
    virtual Gfx::Font& default_fixed_width_font() override;
    virtual FlyString generic_font_name(Web::Platform::GenericFont) override;
    virtual Vector<FlyString> symbol_font_names() override;
    virtual bool is_layout_test_mode() const override { return m_is_layout_test_mode; }

    void update_generic_fonts();

private:
    Vector<FlyString> m_generic_font_names;
    Vector<FlyString> m_symbol_font_names;
    FlyString m_default_font_name;
    RefPtr<Gfx::Font> m_default_fixed_width_font;
    bool m_is_layout_test_mode { false };
};

}
