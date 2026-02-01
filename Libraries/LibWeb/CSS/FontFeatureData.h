/*
 * Copyright (c) 2024, Johan Dahlin <jdahlin@gmail.com>
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FlyString.h>
#include <AK/HashMap.h>
#include <LibGfx/Font/FontVariant.h>
#include <LibGfx/ShapeFeature.h>
#include <LibWeb/CSS/Enums.h>

#pragma once

namespace Web::CSS {

struct FontFeatureData {
    Optional<Gfx::FontVariantAlternates> font_variant_alternates;
    FontVariantCaps font_variant_caps;
    Optional<Gfx::FontVariantEastAsian> font_variant_east_asian;
    FontVariantEmoji font_variant_emoji;
    Optional<Gfx::FontVariantLigatures> font_variant_ligatures;
    Optional<Gfx::FontVariantNumeric> font_variant_numeric;
    FontVariantPosition font_variant_position;

    HashMap<FlyString, u8> font_feature_settings;

    FontKerning font_kerning;
    TextRendering text_rendering;

    Gfx::ShapeFeatures to_shape_features() const;

    bool operator==(FontFeatureData const& other) const = default;
};

}

namespace AK {

template<>
struct Traits<Web::CSS::FontFeatureData> : public DefaultTraits<Web::CSS::FontFeatureData> {
    static unsigned hash(Web::CSS::FontFeatureData const& data)
    {
        u32 hash = 0;
        hash = pair_int_hash(hash, data.font_variant_alternates.has_value() ? Traits<Gfx::FontVariantAlternates>::hash(data.font_variant_alternates.value()) : -1);
        hash = pair_int_hash(hash, to_underlying(data.font_variant_caps));
        hash = pair_int_hash(hash, data.font_variant_east_asian.has_value() ? Traits<Gfx::FontVariantEastAsian>::hash(data.font_variant_east_asian.value()) : -1);
        hash = pair_int_hash(hash, to_underlying(data.font_variant_emoji));
        hash = pair_int_hash(hash, data.font_variant_ligatures.has_value() ? Traits<Gfx::FontVariantLigatures>::hash(data.font_variant_ligatures.value()) : -1);
        hash = pair_int_hash(hash, data.font_variant_numeric.has_value() ? Traits<Gfx::FontVariantNumeric>::hash(data.font_variant_numeric.value()) : -1);
        hash = pair_int_hash(hash, to_underlying(data.font_variant_position));
        hash = pair_int_hash(hash, to_underlying(data.font_kerning));
        hash = pair_int_hash(hash, to_underlying(data.text_rendering));
        for (auto const& [key, value] : data.font_feature_settings)
            hash = pair_int_hash(hash, pair_int_hash(key.hash(), value));
        return hash;
    }
};

}
