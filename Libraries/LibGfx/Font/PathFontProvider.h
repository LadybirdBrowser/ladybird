/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/Typeface.h>

namespace Gfx {

class PathFontProvider final : public SystemFontProvider {
    AK_MAKE_NONCOPYABLE(PathFontProvider);
    AK_MAKE_NONMOVABLE(PathFontProvider);

public:
    PathFontProvider();
    virtual ~PathFontProvider() override;

    void set_name_but_fixme_should_create_custom_system_font_provider(String name) { m_name = move(name); }

    void load_all_fonts_from_uri(StringView);

    virtual RefPtr<Gfx::Font> get_font(FlyString const& family, float point_size, unsigned weight, unsigned width, unsigned slope) override;
    virtual void for_each_typeface_with_family_name(FlyString const& family_name, Function<void(Typeface const&)>) override;
    virtual StringView name() const override { return m_name.bytes_as_string_view(); }

private:
    HashMap<FlyString, Vector<NonnullRefPtr<Typeface>>, AK::ASCIICaseInsensitiveFlyStringTraits> m_typeface_by_family;
    String m_name { "Path"_string };
};

}
