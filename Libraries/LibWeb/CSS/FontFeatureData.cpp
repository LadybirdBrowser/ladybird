/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FontFeatureData.h"
#include <AK/HashMap.h>
#include <LibWeb/CSS/Enums.h>

namespace Web::CSS {

Gfx::ShapeFeatures FontFeatureData::to_shape_features(HashMap<FontFeatureValueKey, Vector<u32>> const& font_feature_values) const
{
    HashMap<FlyString, u8> merged_features;

    auto font_variant_features = [&]() {
        HashMap<FlyString, u8> features;

        // 6.4 https://drafts.csswg.org/css-fonts/#font-variant-ligatures-prop

        auto disable_all_ligatures = [&]() {
            features.set("liga"_fly_string, 0);
            features.set("clig"_fly_string, 0);
            features.set("dlig"_fly_string, 0);
            features.set("hlig"_fly_string, 0);
            features.set("calt"_fly_string, 0);
        };

        if (font_variant_ligatures.has_value()) {
            auto ligature = font_variant_ligatures.value();
            if (ligature.none) {
                // Specifies that all types of ligatures and contextual forms covered by this property are explicitly disabled.
                disable_all_ligatures();
            } else {
                if (ligature.common.has_value()) {
                    switch (ligature.common.value()) {
                    case CommonLigValue::CommonLigatures:
                        // Enables display of common ligatures (OpenType features: liga, clig).
                        features.set("liga"_fly_string, 1);
                        features.set("clig"_fly_string, 1);
                        break;
                    case CommonLigValue::NoCommonLigatures:
                        // Disables display of common ligatures (OpenType features: liga, clig).
                        features.set("liga"_fly_string, 0);
                        features.set("clig"_fly_string, 0);
                        break;
                    }
                }
                if (ligature.discretionary.has_value()) {
                    switch (ligature.discretionary.value()) {
                    case DiscretionaryLigValue::DiscretionaryLigatures:
                        // Enables display of discretionary ligatures (OpenType feature: dlig).
                        features.set("dlig"_fly_string, 1);
                        break;
                    case DiscretionaryLigValue::NoDiscretionaryLigatures:
                        // Disables display of discretionary ligatures (OpenType feature: dlig).
                        features.set("dlig"_fly_string, 0);
                        break;
                    }
                }

                if (ligature.historical.has_value()) {
                    switch (ligature.historical.value()) {
                    case HistoricalLigValue::HistoricalLigatures:
                        // Enables display of historical ligatures (OpenType feature: hlig).
                        features.set("hlig"_fly_string, 1);
                        break;
                    case HistoricalLigValue::NoHistoricalLigatures:
                        // Disables display of historical ligatures (OpenType feature: hlig).
                        features.set("hlig"_fly_string, 0);
                        break;
                    }
                }

                if (ligature.contextual.has_value()) {
                    switch (ligature.contextual.value()) {
                    case ContextualAltValue::Contextual:
                        // Enables display of contextual ligatures (OpenType feature: calt).
                        features.set("calt"_fly_string, 1);
                        break;
                    case ContextualAltValue::NoContextual:
                        // Disables display of contextual ligatures (OpenType feature: calt).
                        features.set("calt"_fly_string, 0);
                        break;
                    }
                }
            }
        } else if (text_rendering == TextRendering::Optimizespeed) {
            // AD-HOC: Disable ligatures if font-variant-ligatures is set to normal and text rendering is set to optimize speed.
            disable_all_ligatures();
        } else {
            // A value of normal specifies that common default features are enabled, as described in detail in the next section.
            features.set("liga"_fly_string, 1);
            features.set("clig"_fly_string, 1);
        }

        // 6.5 https://drafts.csswg.org/css-fonts/#font-variant-position-prop
        switch (font_variant_position) {
        case FontVariantPosition::Normal:
            // None of the features listed below are enabled.
            break;
        case FontVariantPosition::Sub:
            // Enables display of subscripts (OpenType feature: subs).
            features.set("subs"_fly_string, 1);
            break;
        case FontVariantPosition::Super:
            // Enables display of superscripts (OpenType feature: sups).
            features.set("sups"_fly_string, 1);
            break;
        default:
            break;
        }

        // 6.6 https://drafts.csswg.org/css-fonts/#font-variant-caps-prop
        switch (font_variant_caps) {
        case FontVariantCaps::Normal:
            // None of the features listed below are enabled.
            break;
        case FontVariantCaps::SmallCaps:
            // Enables display of small capitals (OpenType feature: smcp). Small-caps glyphs typically use the form of uppercase letters but are reduced to the size of lowercase letters.
            features.set("smcp"_fly_string, 1);
            break;
        case FontVariantCaps::AllSmallCaps:
            // Enables display of small capitals for both upper and lowercase letters (OpenType features: c2sc, smcp).
            features.set("c2sc"_fly_string, 1);
            features.set("smcp"_fly_string, 1);
            break;
        case FontVariantCaps::PetiteCaps:
            // Enables display of petite capitals (OpenType feature: pcap).
            features.set("pcap"_fly_string, 1);
            break;
        case FontVariantCaps::AllPetiteCaps:
            // Enables display of petite capitals for both upper and lowercase letters (OpenType features: c2pc, pcap).
            features.set("c2pc"_fly_string, 1);
            features.set("pcap"_fly_string, 1);
            break;
        case FontVariantCaps::Unicase:
            // Enables display of mixture of small capitals for uppercase letters with normal lowercase letters (OpenType feature: unic).
            features.set("unic"_fly_string, 1);
            break;
        case FontVariantCaps::TitlingCaps:
            // Enables display of titling capitals (OpenType feature: titl).
            features.set("titl"_fly_string, 1);
            break;
        default:
            break;
        }

        // 6.7 https://drafts.csswg.org/css-fonts/#font-variant-numeric-prop
        if (font_variant_numeric.has_value()) {
            auto numeric = font_variant_numeric.value();
            if (numeric.figure == NumericFigureValue::OldstyleNums) {
                // Enables display of old-style numerals (OpenType feature: onum).
                features.set("onum"_fly_string, 1);
            } else if (numeric.figure == NumericFigureValue::LiningNums) {
                // Enables display of lining numerals (OpenType feature: lnum).
                features.set("lnum"_fly_string, 1);
            }

            if (numeric.spacing == NumericSpacingValue::ProportionalNums) {
                // Enables display of proportional numerals (OpenType feature: pnum).
                features.set("pnum"_fly_string, 1);
            } else if (numeric.spacing == NumericSpacingValue::TabularNums) {
                // Enables display of tabular numerals (OpenType feature: tnum).
                features.set("tnum"_fly_string, 1);
            }

            if (numeric.fraction == NumericFractionValue::DiagonalFractions) {
                // Enables display of diagonal fractions (OpenType feature: frac).
                features.set("frac"_fly_string, 1);
            } else if (numeric.fraction == NumericFractionValue::StackedFractions) {
                // Enables display of stacked fractions (OpenType feature: afrc).
                features.set("afrc"_fly_string, 1);
            }

            if (numeric.ordinal) {
                // Enables display of letter forms used with ordinal numbers (OpenType feature: ordn).
                features.set("ordn"_fly_string, 1);
            }
            if (numeric.slashed_zero) {
                // Enables display of slashed zeros (OpenType feature: zero).
                features.set("zero"_fly_string, 1);
            }
        }

        // 6.8 https://drafts.csswg.org/css-fonts/#font-variant-alternates-prop
        // FIXME: These values never apply to generic font families
        if (font_variant_alternates.has_value()) {
            auto alternates = font_variant_alternates.value();
            if (alternates.historical_forms) {
                // Enables display of historical forms (OpenType feature: hist).
                features.set("hist"_fly_string, 1);
            }

            for (auto const& key : alternates.font_feature_value_entries) {

                auto const& maybe_values = font_feature_values.get(key);

                if (!maybe_values.has_value())
                    continue;

                auto const& values = maybe_values.value();

                switch (key.type) {
                case FontFeatureValueType::Stylistic: {
                    // Enables display of stylistic alternates (font specific, OpenType feature: salt <feature-index>).
                    // stylistic(<feature-value-name>)
                    features.set("salt"_fly_string, values[0]);
                    break;
                }
                case FontFeatureValueType::Styleset: {
                    // Enables display with stylistic sets (font specific, OpenType feature: ss<feature-index> OpenType
                    // currently defines ss01 through ss20).

                    // https://drafts.csswg.org/css-fonts/#multi-value-features
                    // For the styleset() property value and @styleset rule, multiple values indicate the style sets to
                    // be enabled. Values between 1 and 99 enable OpenType features ss01 through ss99. However, the
                    // OpenType standard only officially defines ss01 through ss20. For OpenType fonts, values greater
                    // than 99 or equal to 0 do not generate a syntax error when parsed but enable no OpenType features.
                    for (auto const& value : values) {
                        if (value > 99 || value == 0)
                            continue;

                        features.set(MUST(String::formatted("ss{:02}", value)), 1);
                    }

                    break;
                }
                case FontFeatureValueType::CharacterVariant: {
                    // Enables display of specific character variants (font specific, OpenType feature:
                    // cv<feature-index> OpenType currently defines cv01 through cv99).

                    // https://drafts.csswg.org/css-fonts/#multi-value-features
                    // For <@character-variant>, a single value between 1 and 99 indicates the enabling of OpenType
                    // feature cv01 through cv99. For OpenType fonts, values greater than 99 or equal to 0 are ignored
                    // but do not generate a syntax error when parsed but enable no OpenType features. When two values
                    // are listed, the first value indicates the feature used and the second the value passed for that
                    // feature.
                    auto const feature = values[0];

                    if (feature > 99 || feature == 0)
                        break;

                    auto const value = values.size() > 1 ? values[1] : 1;

                    features.set(MUST(String::formatted("cv{:02}", feature)), value);
                    break;
                }
                case FontFeatureValueType::Swash: {
                    // Enables display of swash glyphs (font specific, OpenType feature: swsh <feature-index>,
                    // cswh <feature-index>).
                    features.set("swsh"_fly_string, values[0]);
                    features.set("cswh"_fly_string, values[0]);
                    break;
                }
                case FontFeatureValueType::Ornaments: {
                    // Enables replacement of default glyphs with ornaments, if provided in the font (font specific,
                    // OpenType feature: ornm <feature-index>).
                    features.set("ornm"_fly_string, values[0]);
                    break;
                }
                case FontFeatureValueType::Annotation: {
                    // annotation(<feature-value-name>)
                    // Enables display of alternate annotation forms (font specific, OpenType feature: nalt <feature-index>).
                    features.set("nalt"_fly_string, values[0]);
                    break;
                }
                }
            }
        }

        // 6.10 https://drafts.csswg.org/css-fonts/#font-variant-east-asian-prop
        if (font_variant_east_asian.has_value()) {
            auto east_asian = font_variant_east_asian.value();
            if (east_asian.variant.has_value()) {
                switch (east_asian.variant.value()) {
                case EastAsianVariant::Jis78:
                    // Enables display of JIS78 forms (OpenType feature: jp78).
                    features.set("jp78"_fly_string, 1);
                    break;
                case EastAsianVariant::Jis83:
                    // Enables display of JIS83 forms (OpenType feature: jp83).
                    features.set("jp83"_fly_string, 1);
                    break;
                case EastAsianVariant::Jis90:
                    // Enables display of JIS90 forms (OpenType feature: jp90).
                    features.set("jp90"_fly_string, 1);
                    break;
                case EastAsianVariant::Jis04:
                    // Enables display of JIS04 forms (OpenType feature: jp04).
                    features.set("jp04"_fly_string, 1);
                    break;
                case EastAsianVariant::Simplified:
                    // Enables display of simplified forms (OpenType feature: smpl).
                    features.set("smpl"_fly_string, 1);
                    break;
                case EastAsianVariant::Traditional:
                    // Enables display of traditional forms (OpenType feature: trad).
                    features.set("trad"_fly_string, 1);
                    break;
                }
            }
            if (east_asian.width.has_value()) {
                switch (east_asian.width.value()) {
                case EastAsianWidth::FullWidth:
                    // Enables display of full-width forms (OpenType feature: fwid).
                    features.set("fwid"_fly_string, 1);
                    break;
                case EastAsianWidth::ProportionalWidth:
                    // Enables display of proportional-width forms (OpenType feature: pwid).
                    features.set("pwid"_fly_string, 1);
                    break;
                }
            }
            if (east_asian.ruby) {
                // Enables display of ruby forms (OpenType feature: ruby).
                features.set("ruby"_fly_string, 1);
            }
        }

        // FIXME: vkrn should be enabled for vertical text.
        switch (font_kerning) {
        case FontKerning::Auto:
            // AD-HOC: Disable kerning if font-kerning is set to normal and text rendering is set to optimize speed.
            features.set("kern"_fly_string, text_rendering != TextRendering::Optimizespeed ? 1 : 0);
            break;
        case FontKerning::Normal:
            features.set("kern"_fly_string, 1);
            break;
        case FontKerning::None:
            features.set("kern"_fly_string, 0);
            break;
        default:
            break;
        }

        return features;
    };

    // https://drafts.csswg.org/css-fonts-4/#feature-variation-precedence
    // FIXME: 1. Font features enabled by default are applied, including features required for a given script. See § 7.1
    //           Default features for a description of these.

    // FIXME: 2. Font variations as enabled by the font-weight, font-width, and font-style properties are applied.
    //
    //           The application of the value enabled by font-style is affected by font selection, because this property
    //           might select an italic or an oblique font. The value applied is the closest matching value as
    //           determined by the font matching algorithm. User agents must apply at most one value due to the
    //           font-style property; both "ital" and "slnt" values must not be set together.
    //
    //           If the selected font is defined in an @font-face rule, then the values applied at this step should be
    //           clamped to the value of the font-weight, font-width, and font-style descriptors in that @font-face
    //           rule.
    //
    //           Then, the values applied in this step should be clamped (possibly again) to the values that are
    //           supported by the font.

    // FIXME: 3. The language specified by the inherited value of lang/xml:lang is applied.

    // FIXME: 4. If the font is defined via an @font-face rule, the font language override implied by the
    //           font-language-override descriptor in the @font-face rule is applied.

    // FIXME: 5. If the font is defined via an @font-face rule, that @font-face rule includes at least one valid
    //           font-named-instance descriptor with a value other than 'font-named-instance/none', and the loaded font
    //           resource includes a named instance with that name according to the § 5.1 Localized name matching rules,
    //           then all the variation values represented by that named instance are applied. These values are clamped
    //           to the values that are supported by the font.

    // FIXME: 6. If the font is defined via an @font-face rule, the font variations implied by the
    //           font-variation-settings descriptor in the @font-face rule are applied.

    // FIXME: 7. If the font is defined via an @font-face rule, the font features implied by the font-feature-settings
    //           descriptor in the @font-face rule are applied.

    // FIXME: 8. The font language override implied by the value of the font-language-override property is applied.

    // FIXME: 9. Font variations implied by the value of the font-optical-sizing property are applied.

    // 10. Font features implied by the value of the font-variant property, the related font-variant subproperties and
    //     any other CSS property that uses OpenType features (e.g. the font-kerning property) are applied.
    merged_features.update(font_variant_features());

    // FIXME: 11. Feature settings determined by properties other than font-variant or font-feature-settings are
    //            applied. For example, setting a non-default value for the letter-spacing property disables optional
    //            ligatures.

    // FIXME: 12. Font variations implied by the value of the font-variation-settings property are applied. These values
    //            should be clamped to the values that are supported by the font.

    // 13. Font features implied by the value of font-feature-settings property are applied.
    merged_features.update(font_feature_settings);

    Gfx::ShapeFeatures shape_features;
    shape_features.ensure_capacity(merged_features.size());

    for (auto& it : merged_features) {
        auto key_string_view = it.key.bytes_as_string_view();
        shape_features.unchecked_append({ { key_string_view[0], key_string_view[1], key_string_view[2], key_string_view[3] }, static_cast<u32>(it.value) });
    }

    return shape_features;
}

}
