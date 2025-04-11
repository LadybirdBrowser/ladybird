/*
 * Copyright (c) 2025, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Font/FontDatabase.h>

namespace Gfx {

class FontconfigFontProvider final : public SystemFontProvider {
    AK_MAKE_NONCOPYABLE(FontconfigFontProvider);
    AK_MAKE_NONMOVABLE(FontconfigFontProvider);

public:
    FontconfigFontProvider();
    virtual ~FontconfigFontProvider() override;

    virtual RefPtr<Gfx::Font> get_font(FlyString const& family, float point_size, unsigned weight, unsigned width, unsigned slope) override;
    virtual void for_each_typeface_with_family_name(FlyString const& family_name, Function<void(FontDescription)>) override;
    virtual StringView name() const override { return "FontConfig"sv; }

    void add_uri_to_config(StringView);

private:
    Optional<FontDescription> description_for_fontconfig_parameters(FlyString const& family, ByteString const& path, int index, int weight, int width, int slant);
    RefPtr<Typeface> load_typeface_from_path(ByteString const& path, int index);

    HashMap<FlyString, Vector<NonnullRefPtr<Typeface>>, AK::ASCIICaseInsensitiveFlyStringTraits> m_typeface_by_family;
};

}
