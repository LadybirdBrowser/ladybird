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
#include <LibGfx/Font/Typeface.h>
#include <LibGfx/Forward.h>

namespace Gfx {

struct CodePointFallbackKey {
    u32 code_point { 0 };
    u16 weight { 0 };
    u16 width { 0 };
    u8 slope { 0 };

    bool operator==(CodePointFallbackKey const&) const = default;

    unsigned hash() const
    {
        return pair_int_hash(
            pair_int_hash(code_point, weight),
            pair_int_hash(width, slope));
    }
};

class SystemFontProvider {
public:
    virtual ~SystemFontProvider();

    virtual StringView name() const = 0;
    virtual RefPtr<Gfx::Font> get_font(FlyString const& family, float point_size, unsigned weight, unsigned width, unsigned slope, Optional<FontVariationSettings> const& font_variation_settings = {}, Optional<Gfx::ShapeFeatures> const& shape_features = {}) = 0;
    virtual void for_each_typeface_with_family_name(FlyString const& family_name, Function<void(Typeface const&)>) = 0;
};

class FontDatabase {
public:
    static FontDatabase& the();
    SystemFontProvider& install_system_font_provider(NonnullOwnPtr<SystemFontProvider>);

    RefPtr<Gfx::Font> get(FlyString const& family, float point_size, unsigned weight, unsigned width, unsigned slope, Optional<FontVariationSettings> const& font_variation_settings = {}, Optional<Gfx::ShapeFeatures> const& shape_features = {});
    RefPtr<Gfx::Font> get_font_for_code_point(u32 code_point, float point_size, u16 weight, u16 width, u8 slope);
    void for_each_typeface_with_family_name(FlyString const& family_name, Function<void(Typeface const&)>);
    [[nodiscard]] StringView system_font_provider_name() const;

    static ErrorOr<Vector<String>> font_directories();

private:
    FontDatabase();
    ~FontDatabase() = default;

    OwnPtr<SystemFontProvider> m_system_font_provider;

    struct CodePointFallbackEntry {
        FlyString family_name;
        RefPtr<Typeface const> typeface;
    };
    HashMap<CodePointFallbackKey, CodePointFallbackEntry> m_code_point_fallback_cache;
};

}

template<>
struct AK::Traits<Gfx::CodePointFallbackKey> : public AK::DefaultTraits<Gfx::CodePointFallbackKey> {
    static unsigned hash(Gfx::CodePointFallbackKey const& key)
    {
        return key.hash();
    }
};
