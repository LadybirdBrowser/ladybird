/*
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CSSFontFaceDescriptors.h>
#include <LibWeb/CSS/CSSRule.h>
#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/CSS/ParsedFontFace.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontSourceStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpenTypeTaggedStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/UnicodeRangeStyleValue.h>
#include <LibWeb/DOM/Document.h>

namespace Web::CSS {

static FlyString extract_font_name(StyleValue const& value)
{
    if (value.is_string())
        return value.as_string().string_value();
    if (value.is_custom_ident())
        return value.as_custom_ident().custom_ident();
    return FlyString {};
}

Vector<ParsedFontFace::Source> ParsedFontFace::sources_from_style_value(StyleValue const& style_value)
{
    Vector<Source> sources;
    auto add_source = [&sources](FontSourceStyleValue const& font_source) {
        font_source.source().visit(
            [&](FontSourceStyleValue::Local const& local) {
                sources.empend(extract_font_name(local.name), OptionalNone {}, Vector<FontTech> {});
            },
            [&](URL const& url) {
                sources.empend(url, font_source.format(), font_source.tech());
            });
    };

    if (style_value.is_font_source()) {
        add_source(style_value.as_font_source());
    } else if (style_value.is_value_list()) {
        for (auto const& source : style_value.as_value_list().values())
            add_source(source->as_font_source());
    }
    return sources;
}

ParsedFontFace ParsedFontFace::from_descriptors(CSSFontFaceDescriptors const& descriptors)
{
    auto extract_percentage_or_normal = [](StyleValue const& value) -> Optional<Percentage> {
        if (value.is_percentage())
            return value.as_percentage().percentage();
        if (value.is_calculated()) {
            // FIXME: These should probably be simplified already?
            return value.as_calculated().resolve_percentage({});
        }
        if (value.to_keyword() == Keyword::Normal)
            return {};

        return {};
    };

    FlyString font_family;
    if (auto value = descriptors.descriptor_or_initial_value(DescriptorID::FontFamily))
        font_family = extract_font_name(*value);

    Optional<int> weight;
    if (auto value = descriptors.descriptor_or_initial_value(DescriptorID::FontWeight)) {
        // https://drafts.csswg.org/css-fonts-4/#font-prop-desc
        // The auto values for these three descriptors have the following effects:
        //  - For font selection purposes, the font is selected as if the appropriate normal value (normal, normal or normal) is chosen
        //  - FIXME: For variation axis clamping, clamping does not occur
        if (value->to_keyword() == Keyword::Auto)
            weight = 400;
        else
            // NOTE: The value we pass here for inherited_font_weight is irrelevant as relative keywords (lighter, bolder) should be disallowed at parse time
            weight = StyleComputer::compute_font_weight(*value, 0, Length::ResolutionContext::for_window(*descriptors.parent_rule()->parent_style_sheet()->owning_document()->window()))->as_number().number();
    }

    Optional<int> slope;
    if (auto value = descriptors.descriptor_or_initial_value(DescriptorID::FontStyle)) {
        // https://drafts.csswg.org/css-fonts-4/#font-prop-desc
        // The auto values for these three descriptors have the following effects:
        //  - For font selection purposes, the font is selected as if the appropriate normal value (normal, normal or normal) is chosen
        //  - FIXME: For variation axis clamping, clamping does not occur
        if (value->to_keyword() == Keyword::Auto)
            slope = 0;
        else
            slope = StyleComputer::compute_font_style(*value, Length::ResolutionContext::for_window(*descriptors.parent_rule()->parent_style_sheet()->owning_document()->window()))->as_font_style().to_font_slope();
    }

    Optional<int> width;
    if (auto value = descriptors.descriptor_or_initial_value(DescriptorID::FontWidth)) {
        // https://drafts.csswg.org/css-fonts-4/#font-prop-desc
        // The auto values for these three descriptors have the following effects:
        //  - For font selection purposes, the font is selected as if the appropriate normal value (normal, normal or normal) is chosen
        //  - FIXME: For variation axis clamping, clamping does not occur
        if (value->to_keyword() == Keyword::Auto)
            width = 100;
        else
            width = StyleComputer::compute_font_width(*value, Length::ResolutionContext::for_window(*descriptors.parent_rule()->parent_style_sheet()->owning_document()->window()))->as_percentage().raw_value();
    }

    Vector<Source> sources;
    if (auto value = descriptors.descriptor_or_initial_value(DescriptorID::Src))
        sources = sources_from_style_value(*value);

    Vector<Gfx::UnicodeRange> unicode_ranges;
    if (auto value = descriptors.descriptor_or_initial_value(DescriptorID::UnicodeRange)) {
        if (value->is_unicode_range()) {
            unicode_ranges.append(value->as_unicode_range().unicode_range());
        } else if (value->is_value_list()) {
            for (auto const& range : value->as_value_list().values())
                unicode_ranges.append(range->as_unicode_range().unicode_range());
        }
    }

    Optional<Percentage> ascent_override;
    if (auto value = descriptors.descriptor_or_initial_value(DescriptorID::AscentOverride))
        ascent_override = extract_percentage_or_normal(*value);

    Optional<Percentage> descent_override;
    if (auto value = descriptors.descriptor_or_initial_value(DescriptorID::DescentOverride))
        descent_override = extract_percentage_or_normal(*value);

    Optional<Percentage> line_gap_override;
    if (auto value = descriptors.descriptor_or_initial_value(DescriptorID::LineGapOverride))
        line_gap_override = extract_percentage_or_normal(*value);

    FontDisplay font_display;
    if (auto value = descriptors.descriptor_or_initial_value(DescriptorID::FontDisplay))
        font_display = keyword_to_font_display(value->to_keyword()).value_or(FontDisplay::Auto);

    Optional<FlyString> font_named_instance;
    if (auto value = descriptors.descriptor_or_initial_value(DescriptorID::FontNamedInstance)) {
        if (value->is_string())
            font_named_instance = value->as_string().string_value();
    }

    Optional<FlyString> font_language_override;
    if (auto value = descriptors.descriptor_or_initial_value(DescriptorID::FontLanguageOverride)) {
        if (value->is_string())
            font_language_override = value->as_string().string_value();
    }

    Optional<OrderedHashMap<FlyString, i64>> font_feature_settings;
    if (auto value = descriptors.descriptor_or_initial_value(DescriptorID::FontFeatureSettings)) {
        if (value->to_keyword() == Keyword::Normal) {
            font_feature_settings.clear();
        } else if (value->is_value_list()) {
            auto const& feature_tags = value->as_value_list().values();
            OrderedHashMap<FlyString, i64> settings;
            settings.ensure_capacity(feature_tags.size());
            for (auto const& feature_tag : feature_tags) {
                auto const& setting_value = feature_tag->as_open_type_tagged().value();
                if (setting_value->is_integer()) {
                    settings.set(feature_tag->as_open_type_tagged().tag(), setting_value->as_integer().integer());
                } else if (setting_value->is_calculated() && setting_value->as_calculated().resolves_to_number()) {
                    if (auto integer = setting_value->as_calculated().resolve_integer({}); integer.has_value()) {
                        settings.set(feature_tag->as_open_type_tagged().tag(), *integer);
                    }
                }
            }
            font_feature_settings = move(settings);
        }
    }

    Optional<OrderedHashMap<FlyString, double>> font_variation_settings;
    if (auto value = descriptors.descriptor_or_initial_value(DescriptorID::FontVariationSettings)) {
        if (value->to_keyword() == Keyword::Normal) {
            font_variation_settings.clear();
        } else if (value->is_value_list()) {
            auto const& variation_tags = value->as_value_list().values();
            OrderedHashMap<FlyString, double> settings;
            settings.ensure_capacity(variation_tags.size());
            for (auto const& variation_tag : variation_tags) {
                auto const& setting_value = variation_tag->as_open_type_tagged().value();
                if (setting_value->is_number()) {
                    settings.set(variation_tag->as_open_type_tagged().tag(), setting_value->as_number().number());
                } else if (setting_value->is_calculated() && setting_value->as_calculated().resolves_to_number()) {
                    if (auto number = setting_value->as_calculated().resolve_number({}); number.has_value()) {
                        settings.set(variation_tag->as_open_type_tagged().tag(), *number);
                    }
                }
            }
            font_variation_settings = move(settings);
        }
    }

    return ParsedFontFace {
        descriptors.parent_rule()->parent_style_sheet(),
        move(font_family),
        move(weight),
        move(slope),
        move(width),
        move(sources),
        move(unicode_ranges),
        move(ascent_override),
        move(descent_override),
        move(line_gap_override),
        move(font_display),
        move(font_named_instance),
        move(font_language_override),
        move(font_feature_settings),
        move(font_variation_settings)
    };
}

ParsedFontFace::ParsedFontFace(GC::Ptr<CSSStyleSheet> parent_style_sheet, FlyString font_family, Optional<int> weight, Optional<int> slope, Optional<int> width, Vector<Source> sources, Vector<Gfx::UnicodeRange> unicode_ranges, Optional<Percentage> ascent_override, Optional<Percentage> descent_override, Optional<Percentage> line_gap_override, FontDisplay font_display, Optional<FlyString> font_named_instance, Optional<FlyString> font_language_override, Optional<OrderedHashMap<FlyString, i64>> font_feature_settings, Optional<OrderedHashMap<FlyString, double>> font_variation_settings)
    : m_parent_style_sheet(parent_style_sheet)
    , m_font_family(move(font_family))
    , m_font_named_instance(move(font_named_instance))
    , m_weight(weight)
    , m_slope(slope)
    , m_width(width)
    , m_sources(move(sources))
    , m_unicode_ranges(move(unicode_ranges))
    , m_ascent_override(move(ascent_override))
    , m_descent_override(move(descent_override))
    , m_line_gap_override(move(line_gap_override))
    , m_font_display(font_display)
    , m_font_language_override(font_language_override)
    , m_font_feature_settings(move(font_feature_settings))
    , m_font_variation_settings(move(font_variation_settings))
{
}

}
