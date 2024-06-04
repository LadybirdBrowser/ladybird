/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/FlyString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <LibGfx/Font/FontWeight.h>
#include <LibGfx/Font/Typeface.h>
#include <LibGfx/Forward.h>

namespace Gfx {

class FontDatabase {
public:
    static FontDatabase& the();

    RefPtr<Gfx::Font> get(FlyString const& family, float point_size, unsigned weight, unsigned width, unsigned slope);
    RefPtr<Gfx::Font> get(FlyString const& family, FlyString const& variant, float point_size);

    void for_each_typeface_with_family_name(FlyString const& family_name, Function<void(Typeface const&)>);

    void load_all_fonts_from_uri(StringView);

private:
    FontDatabase();
    ~FontDatabase() = default;

    RefPtr<Typeface> get_or_create_typeface(FlyString const& family, FlyString const& variant);

    struct Private;
    OwnPtr<Private> m_private;
};

}
