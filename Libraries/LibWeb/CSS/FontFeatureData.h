/*
 * Copyright (c) 2024, Johan Dahlin <jdahlin@gmail.com>
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FlyString.h>
#include <AK/HashMap.h>
#include <LibGfx/ShapeFeature.h>
#include <LibWeb/Forward.h>

#pragma once

namespace Web::CSS {

struct FontVariantAlternates {
    bool historical_forms = false;

    bool operator==(FontVariantAlternates const&) const = default;
};

struct FontVariantEastAsian {
    bool ruby = false;
    Optional<EastAsianVariant> variant;
    Optional<EastAsianWidth> width;

    bool operator==(FontVariantEastAsian const&) const = default;
};

struct FontVariantLigatures {
    bool none = false;
    Optional<CommonLigValue> common {};
    Optional<DiscretionaryLigValue> discretionary {};
    Optional<HistoricalLigValue> historical {};
    Optional<ContextualAltValue> contextual {};

    bool operator==(FontVariantLigatures const&) const = default;
};

struct FontVariantNumeric {
    bool ordinal = false;
    bool slashed_zero = false;
    Optional<NumericFigureValue> figure {};
    Optional<NumericSpacingValue> spacing {};
    Optional<NumericFractionValue> fraction {};

    bool operator==(FontVariantNumeric const&) const = default;
};

struct FontFeatureData {
    Optional<FontVariantAlternates> font_variant_alternates;
    FontVariantCaps font_variant_caps;
    Optional<FontVariantEastAsian> font_variant_east_asian;
    FontVariantEmoji font_variant_emoji;
    Optional<FontVariantLigatures> font_variant_ligatures;
    Optional<FontVariantNumeric> font_variant_numeric;
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
struct Traits<Web::CSS::FontVariantAlternates> : public DefaultTraits<Web::CSS::FontVariantAlternates> {
    static unsigned hash(Web::CSS::FontVariantAlternates const& data)
    {
        u32 hash = data.historical_forms ? 1 : 0;
        return hash;
    }
};

template<>
struct Traits<Web::CSS::FontVariantEastAsian> : public DefaultTraits<Web::CSS::FontVariantEastAsian> {
    static unsigned hash(Web::CSS::FontVariantEastAsian const& data)
    {
        u32 hash = data.ruby ? 1 : 0;
        hash = pair_int_hash(hash, data.variant.has_value() ? to_underlying(data.variant.value()) : -1);
        hash = pair_int_hash(hash, data.width.has_value() ? to_underlying(data.width.value()) : -1);
        return hash;
    }
};

template<>
struct Traits<Web::CSS::FontVariantLigatures> : public DefaultTraits<Web::CSS::FontVariantLigatures> {
    static unsigned hash(Web::CSS::FontVariantLigatures const& data)
    {
        u32 hash = data.none ? 1 : 0;
        hash = pair_int_hash(hash, data.common.has_value() ? to_underlying(data.common.value()) : -1);
        hash = pair_int_hash(hash, data.discretionary.has_value() ? to_underlying(data.discretionary.value()) : -1);
        hash = pair_int_hash(hash, data.historical.has_value() ? to_underlying(data.historical.value()) : -1);
        hash = pair_int_hash(hash, data.contextual.has_value() ? to_underlying(data.contextual.value()) : -1);
        return hash;
    }
};

template<>
struct Traits<Web::CSS::FontVariantNumeric> : public DefaultTraits<Web::CSS::FontVariantNumeric> {
    static unsigned hash(Web::CSS::FontVariantNumeric const& data)
    {
        u32 hash = data.ordinal ? 1 : 0;
        hash = pair_int_hash(hash, data.slashed_zero ? 1 : 0);
        hash = pair_int_hash(hash, data.figure.has_value() ? to_underlying(data.figure.value()) : -1);
        hash = pair_int_hash(hash, data.spacing.has_value() ? to_underlying(data.spacing.value()) : -1);
        hash = pair_int_hash(hash, data.fraction.has_value() ? to_underlying(data.fraction.value()) : -1);
        return hash;
    }
};

template<>
struct Traits<Web::CSS::FontFeatureData> : public DefaultTraits<Web::CSS::FontFeatureData> {
    static unsigned hash(Web::CSS::FontFeatureData const& data)
    {
        u32 hash = 0;
        hash = pair_int_hash(hash, data.font_variant_alternates.has_value() ? Traits<Web::CSS::FontVariantAlternates>::hash(data.font_variant_alternates.value()) : -1);
        hash = pair_int_hash(hash, to_underlying(data.font_variant_caps));
        hash = pair_int_hash(hash, data.font_variant_east_asian.has_value() ? Traits<Web::CSS::FontVariantEastAsian>::hash(data.font_variant_east_asian.value()) : -1);
        hash = pair_int_hash(hash, to_underlying(data.font_variant_emoji));
        hash = pair_int_hash(hash, data.font_variant_ligatures.has_value() ? Traits<Web::CSS::FontVariantLigatures>::hash(data.font_variant_ligatures.value()) : -1);
        hash = pair_int_hash(hash, data.font_variant_numeric.has_value() ? Traits<Web::CSS::FontVariantNumeric>::hash(data.font_variant_numeric.value()) : -1);
        hash = pair_int_hash(hash, to_underlying(data.font_variant_position));
        hash = pair_int_hash(hash, to_underlying(data.font_kerning));
        hash = pair_int_hash(hash, to_underlying(data.text_rendering));
        for (auto const& [key, value] : data.font_feature_settings)
            hash = pair_int_hash(hash, pair_int_hash(key.hash(), value));
        return hash;
    }
};

}
