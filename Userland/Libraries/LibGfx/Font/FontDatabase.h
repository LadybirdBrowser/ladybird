/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <LibGfx/Font/Typeface.h>
#include <LibGfx/Forward.h>

namespace Gfx {

class SystemFontProvider {
public:
    virtual ~SystemFontProvider();

    virtual StringView name() const = 0;
    virtual RefPtr<Gfx::Font> get_font(FlyString const& family, float point_size, unsigned weight, unsigned width, unsigned slope) = 0;
    virtual void for_each_typeface_with_family_name(FlyString const& family_name, Function<void(Typeface const&)>) = 0;
};

class FontDatabase {
public:
    static FontDatabase& the();
    SystemFontProvider& install_system_font_provider(NonnullOwnPtr<SystemFontProvider>);

    RefPtr<Gfx::Font> get(FlyString const& family, float point_size, unsigned weight, unsigned width, unsigned slope);
    void for_each_typeface_with_family_name(FlyString const& family_name, Function<void(Typeface const&)>);
    [[nodiscard]] StringView system_font_provider_name() const;

private:
    FontDatabase();
    ~FontDatabase() = default;

    OwnPtr<SystemFontProvider> m_system_font_provider;
};

}
