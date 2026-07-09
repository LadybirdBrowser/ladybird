/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Font/Typeface.h>
#include <LibGfx/Font/TypefaceSkia.h>

namespace Gfx {

class SkiaFontProvider final : public SystemFontProvider {
    AK_MAKE_NONCOPYABLE(SkiaFontProvider);
    AK_MAKE_NONMOVABLE(SkiaFontProvider);

public:
    SkiaFontProvider();
    virtual ~SkiaFontProvider() override;

    void load_all_fonts_from_uri(StringView);

    virtual RefPtr<Gfx::Font> get_font(FlyString const& family, float point_size, unsigned weight, unsigned width, unsigned slope, Optional<FontVariationSettings> const& font_variation_settings = {}, Optional<Gfx::ShapeFeatures> const& shape_features = {}) override;
    virtual void for_each_typeface_with_family_name(FlyString const& family_name, Function<void(Typeface const&)>) override;
    virtual StringView name() const LIFETIME_BOUND override { return "Skia"sv; }

private:
    Vector<NonnullRefPtr<Typeface>> const& ensure_system_family_cached(FlyString const& family_name);
    HashMap<FlyString, Vector<NonnullRefPtr<Typeface>>, AK::ASCIICaseInsensitiveFlyStringTraits> m_bundled_typefaces;
    HashMap<FlyString, Vector<NonnullRefPtr<Typeface>>, AK::ASCIICaseInsensitiveFlyStringTraits> m_system_typefaces;
};

}
