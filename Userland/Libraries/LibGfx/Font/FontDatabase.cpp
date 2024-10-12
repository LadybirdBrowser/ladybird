/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FlyString.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontDatabase.h>

namespace Gfx {

// Key function for SystemFontProvider to emit the vtable here
SystemFontProvider::~SystemFontProvider() = default;

FontDatabase& FontDatabase::the()
{
    static FontDatabase s_the;
    return s_the;
}

SystemFontProvider& FontDatabase::install_system_font_provider(NonnullOwnPtr<SystemFontProvider> provider)
{
    VERIFY(!m_system_font_provider);
    m_system_font_provider = move(provider);
    return *m_system_font_provider;
}

StringView FontDatabase::system_font_provider_name() const
{
    VERIFY(m_system_font_provider);
    return m_system_font_provider->name();
}

FontDatabase::FontDatabase() = default;

RefPtr<Gfx::Font> FontDatabase::get(FlyString const& family, float point_size, unsigned weight, unsigned width, unsigned slope)
{
    return m_system_font_provider->get_font(family, point_size, weight, width, slope);
}

void FontDatabase::for_each_typeface_with_family_name(FlyString const& family_name, Function<void(Typeface const&)> callback)
{
    m_system_font_provider->for_each_typeface_with_family_name(family_name, move(callback));
}

}
