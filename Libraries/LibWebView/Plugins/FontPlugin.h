/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWebView/Forward.h>

namespace WebView {

struct GenericFontKey {
    Web::Platform::GenericFont generic_font;
    int weight;
    int slope;

    bool operator==(GenericFontKey const&) const = default;
};

class WEBVIEW_API FontPlugin final : public Web::Platform::FontPlugin {
public:
    FontPlugin(bool is_layout_test_mode, Gfx::SystemFontProvider* = nullptr);
    virtual ~FontPlugin();

    virtual RefPtr<Gfx::Font> default_font(float point_size) override;
    virtual Gfx::Font& default_fixed_width_font() override;
    virtual FlyString generic_font_name(Web::Platform::GenericFont, int weight, int slope) override;
    virtual Vector<FlyString> symbol_font_names() override;
    virtual bool is_layout_test_mode() const override { return m_is_layout_test_mode; }

    void update_generic_fonts();

private:
    FlyString compute_generic_font_name(Web::Platform::GenericFont, int weight, int slope);

    Vector<Vector<FlyString>> m_generic_font_fallbacks;
    HashMap<GenericFontKey, FlyString> m_generic_font_cache;
    Vector<FlyString> m_symbol_font_names;
    RefPtr<Gfx::Font> m_default_fixed_width_font;
    bool m_is_layout_test_mode { false };
};

}

template<>
struct AK::Traits<WebView::GenericFontKey> : public AK::DefaultTraits<WebView::GenericFontKey> {
    static unsigned hash(WebView::GenericFontKey const& key)
    {
        return pair_int_hash(pair_int_hash(to_underlying(key.generic_font), key.weight), key.slope);
    }
};
