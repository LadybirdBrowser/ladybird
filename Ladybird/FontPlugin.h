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

namespace Ladybird {

class FontPlugin final : public Web::Platform::FontPlugin {
public:
    FontPlugin(bool is_layout_test_mode, Gfx::SystemFontProvider* = nullptr);
    virtual ~FontPlugin();

    virtual Gfx::Font& default_font() override;
    virtual Gfx::Font& default_fixed_width_font() override;
    virtual RefPtr<Gfx::Font> default_emoji_font(float point_size) override;
    virtual FlyString generic_font_name(Web::Platform::GenericFont) override;

    void update_generic_fonts();

private:
    Vector<FlyString> m_generic_font_names;
    RefPtr<Gfx::Font> m_default_font;
    RefPtr<Gfx::Font> m_default_fixed_width_font;
    bool m_is_layout_test_mode { false };
};

}
