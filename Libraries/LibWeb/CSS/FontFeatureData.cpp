/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FontFeatureData.h"
#include <AK/HashMap.h>

namespace Web::CSS {

Gfx::ShapeFeatures FontFeatureData::to_shape_features() const
{
    HashMap<StringView, u8> merged_features;

    auto font_variant_features = [&]() {
        HashMap<StringView, u8> features;

        // 6.4 https://drafts.csswg.org/css-fonts/#font-variant-ligatures-prop

        auto disable_all_ligatures = [&]() {
            features.set("liga"sv, 0);
            features.set("clig"sv, 0);
            features.set("dlig"sv, 0);
            features.set("hlig"sv, 0);
            features.set("calt"sv, 0);
        };

        if (font_variant_ligatures.has_value()) {
            auto ligature = font_variant_ligatures.value();
            if (ligature.none) {
                // Specifies that all types of ligatures and contextual forms covered by this property are explicitly disabled.
                disable_all_ligatures();
            } else {
                switch (ligature.common) {
                case Gfx::FontVariantLigatures::Common::Common:
                    // Enables display of common ligatures (OpenType features: liga, clig).
                    features.set("liga"sv, 1);
                    features.set("clig"sv, 1);
                    break;
                case Gfx::FontVariantLigatures::Common::NoCommon:
                    // Disables display of common ligatures (OpenType features: liga, clig).
                    features.set("liga"sv, 0);
                    features.set("clig"sv, 0);
                    break;
                case Gfx::FontVariantLigatures::Common::Unset:
                    break;
                }

                switch (ligature.discretionary) {
                case Gfx::FontVariantLigatures::Discretionary::Discretionary:
                    // Enables display of discretionary ligatures (OpenType feature: dlig).
                    features.set("dlig"sv, 1);
                    break;
                case Gfx::FontVariantLigatures::Discretionary::NoDiscretionary:
                    // Disables display of discretionary ligatures (OpenType feature: dlig).
                    features.set("dlig"sv, 0);
                    break;
                case Gfx::FontVariantLigatures::Discretionary::Unset:
                    break;
                }

                switch (ligature.historical) {
                case Gfx::FontVariantLigatures::Historical::Historical:
                    // Enables display of historical ligatures (OpenType feature: hlig).
                    features.set("hlig"sv, 1);
                    break;
                case Gfx::FontVariantLigatures::Historical::NoHistorical:
                    // Disables display of historical ligatures (OpenType feature: hlig).
                    features.set("hlig"sv, 0);
                    break;
                case Gfx::FontVariantLigatures::Historical::Unset:
                    break;
                }

                switch (ligature.contextual) {
                case Gfx::FontVariantLigatures::Contextual::Contextual:
                    // Enables display of contextual ligatures (OpenType feature: calt).
                    features.set("calt"sv, 1);
                    break;
                case Gfx::FontVariantLigatures::Contextual::NoContextual:
                    // Disables display of contextual ligatures (OpenType feature: calt).
                    features.set("calt"sv, 0);
                    break;
                case Gfx::FontVariantLigatures::Contextual::Unset:
                    break;
                }
            }
        } else if (text_rendering == TextRendering::Optimizespeed) {
            // AD-HOC: Disable ligatures if font-variant-ligatures is set to normal and text rendering is set to optimize speed.
            disable_all_ligatures();
        } else {
            // A value of normal specifies that common default features are enabled, as described in detail in the next section.
            features.set("liga"sv, 1);
            features.set("clig"sv, 1);
        }

        // 6.5 https://drafts.csswg.org/css-fonts/#font-variant-position-prop
        switch (font_variant_position) {
        case FontVariantPosition::Normal:
            // None of the features listed below are enabled.
            break;
        case FontVariantPosition::Sub:
            // Enables display of subscripts (OpenType feature: subs).
            features.set("subs"sv, 1);
            break;
        case FontVariantPosition::Super:
            // Enables display of superscripts (OpenType feature: sups).
            features.set("sups"sv, 1);
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
            features.set("smcp"sv, 1);
            break;
        case FontVariantCaps::AllSmallCaps:
            // Enables display of small capitals for both upper and lowercase letters (OpenType features: c2sc, smcp).
            features.set("c2sc"sv, 1);
            features.set("smcp"sv, 1);
            break;
        case FontVariantCaps::PetiteCaps:
            // Enables display of petite capitals (OpenType feature: pcap).
            features.set("pcap"sv, 1);
            break;
        case FontVariantCaps::AllPetiteCaps:
            // Enables display of petite capitals for both upper and lowercase letters (OpenType features: c2pc, pcap).
            features.set("c2pc"sv, 1);
            features.set("pcap"sv, 1);
            break;
        case FontVariantCaps::Unicase:
            // Enables display of mixture of small capitals for uppercase letters with normal lowercase letters (OpenType feature: unic).
            features.set("unic"sv, 1);
            break;
        case FontVariantCaps::TitlingCaps:
            // Enables display of titling capitals (OpenType feature: titl).
            features.set("titl"sv, 1);
            break;
        default:
            break;
        }

        // 6.7 https://drafts.csswg.org/css-fonts/#font-variant-numeric-prop
        if (font_variant_numeric.has_value()) {
            auto numeric = font_variant_numeric.value();
            if (numeric.figure == Gfx::FontVariantNumeric::Figure::Oldstyle) {
                // Enables display of old-style numerals (OpenType feature: onum).
                features.set("onum"sv, 1);
            } else if (numeric.figure == Gfx::FontVariantNumeric::Figure::Lining) {
                // Enables display of lining numerals (OpenType feature: lnum).
                features.set("lnum"sv, 1);
            }

            if (numeric.spacing == Gfx::FontVariantNumeric::Spacing::Proportional) {
                // Enables display of proportional numerals (OpenType feature: pnum).
                features.set("pnum"sv, 1);
            } else if (numeric.spacing == Gfx::FontVariantNumeric::Spacing::Tabular) {
                // Enables display of tabular numerals (OpenType feature: tnum).
                features.set("tnum"sv, 1);
            }

            if (numeric.fraction == Gfx::FontVariantNumeric::Fraction::Diagonal) {
                // Enables display of diagonal fractions (OpenType feature: frac).
                features.set("frac"sv, 1);
            } else if (numeric.fraction == Gfx::FontVariantNumeric::Fraction::Stacked) {
                // Enables display of stacked fractions (OpenType feature: afrc).
                features.set("afrc"sv, 1);
                features.set("afrc"sv, 1);
            }

            if (numeric.ordinal) {
                // Enables display of letter forms used with ordinal numbers (OpenType feature: ordn).
                features.set("ordn"sv, 1);
            }
            if (numeric.slashed_zero) {
                // Enables display of slashed zeros (OpenType feature: zero).
                features.set("zero"sv, 1);
            }
        }

        // FIXME: 6.8 https://drafts.csswg.org/css-fonts/#font-variant-alternates-prop

        // 6.10 https://drafts.csswg.org/css-fonts/#font-variant-east-asian-prop
        if (font_variant_east_asian.has_value()) {
            auto east_asian = font_variant_east_asian.value();
            switch (east_asian.variant) {
            case Gfx::FontVariantEastAsian::Variant::Jis78:
                // Enables display of JIS78 forms (OpenType feature: jp78).
                features.set("jp78"sv, 1);
                break;
            case Gfx::FontVariantEastAsian::Variant::Jis83:
                // Enables display of JIS83 forms (OpenType feature: jp83).
                features.set("jp83"sv, 1);
                break;
            case Gfx::FontVariantEastAsian::Variant::Jis90:
                // Enables display of JIS90 forms (OpenType feature: jp90).
                features.set("jp90"sv, 1);
                break;
            case Gfx::FontVariantEastAsian::Variant::Jis04:
                // Enables display of JIS04 forms (OpenType feature: jp04).
                features.set("jp04"sv, 1);
                break;
            case Gfx::FontVariantEastAsian::Variant::Simplified:
                // Enables display of simplified forms (OpenType feature: smpl).
                features.set("smpl"sv, 1);
                break;
            case Gfx::FontVariantEastAsian::Variant::Traditional:
                // Enables display of traditional forms (OpenType feature: trad).
                features.set("trad"sv, 1);
                break;
            default:
                break;
            }
            switch (east_asian.width) {
            case Gfx::FontVariantEastAsian::Width::FullWidth:
                // Enables display of full-width forms (OpenType feature: fwid).
                features.set("fwid"sv, 1);
                break;
            case Gfx::FontVariantEastAsian::Width::Proportional:
                // Enables display of proportional-width forms (OpenType feature: pwid).
                features.set("pwid"sv, 1);
                break;
            default:
                break;
            }
            if (east_asian.ruby) {
                // Enables display of ruby forms (OpenType feature: ruby).
                features.set("ruby"sv, 1);
            }
        }

        // FIXME: vkrn should be enabled for vertical text.
        switch (font_kerning) {
        case FontKerning::Auto:
            // AD-HOC: Disable kerning if font-kerning is set to normal and text rendering is set to optimize speed.
            features.set("kern"sv, text_rendering != TextRendering::Optimizespeed ? 1 : 0);
            break;
        case FontKerning::Normal:
            features.set("kern"sv, 1);
            break;
        case FontKerning::None:
            features.set("kern"sv, 0);
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
    for (auto const& [key, value] : font_feature_settings)
        merged_features.set(key.bytes_as_string_view(), value);

    Gfx::ShapeFeatures shape_features;
    shape_features.ensure_capacity(merged_features.size());

    for (auto& it : merged_features)
        shape_features.unchecked_append({ { it.key[0], it.key[1], it.key[2], it.key[3] }, static_cast<u32>(it.value) });

    return shape_features;
}

}
