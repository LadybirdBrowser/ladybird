/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/Function.h>
#include <AK/HashFunctions.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <LibGfx/Font/Typeface.h>
#include <LibGfx/Forward.h>

namespace Gfx {

class SystemFontProvider {
public:
    virtual ~SystemFontProvider();

    virtual StringView name() const = 0;
    virtual RefPtr<Gfx::Font> get_font(FlyString const& family, float point_size, unsigned weight, unsigned width, unsigned slope, Optional<FontVariationSettings> const& font_variation_settings = {}, Optional<Gfx::ShapeFeatures> const& shape_features = {}) = 0;
    virtual void for_each_typeface_with_family_name(FlyString const& family_name, Function<void(Typeface const&)>) = 0;
    virtual RefPtr<Typeface> get_typeface_by_id(u64 generation, u64 face_id);
    virtual RefPtr<Gfx::Font> get_font_for_code_point(u32 code_point, float point_size, u16 weight, u16 width, u8 slope, bool prefer_color_emoji);
    virtual Optional<FlyString> resolve_generic_family(StringView family_name, u16 weight, u8 slope);
};

class FontDatabase {
public:
    static FontDatabase& the();
    SystemFontProvider& install_system_font_provider(NonnullOwnPtr<SystemFontProvider>);

    RefPtr<Gfx::Font> get(FlyString const& family, float point_size, unsigned weight, unsigned width, unsigned slope, Optional<FontVariationSettings> const& font_variation_settings = {}, Optional<Gfx::ShapeFeatures> const& shape_features = {});
    RefPtr<Gfx::Font> get_font_for_code_point(u32 code_point, float point_size, u16 weight, u16 width, u8 slope, bool prefer_color_emoji);
    RefPtr<Typeface> get_typeface_by_id(u64 generation, u64 face_id);
    Optional<FlyString> resolve_generic_family(StringView family_name, u16 weight, u8 slope);
    void for_each_typeface_with_family_name(FlyString const& family_name, Function<void(Typeface const&)>);
    [[nodiscard]] StringView system_font_provider_name() const;
    [[nodiscard]] bool has_system_font_provider() const { return m_system_font_provider; }

    void set_force_freetype_rasterization(bool force) { m_force_freetype_rasterization = force; }
    [[nodiscard]] bool force_freetype_rasterization() const { return m_force_freetype_rasterization; }

    static ErrorOr<Vector<String>> font_directories();

private:
    FontDatabase();
    ~FontDatabase() = default;

    OwnPtr<SystemFontProvider> m_system_font_provider;
    bool m_force_freetype_rasterization { false };
};

}
