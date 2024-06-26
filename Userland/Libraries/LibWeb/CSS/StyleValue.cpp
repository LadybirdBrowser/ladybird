/*
 * Copyright (c) 2018-2023, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/FontStyleMapping.h>
#include <LibGfx/Font/FontWeight.h>
#include <LibWeb/CSS/StyleValue.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/BackgroundRepeatStyleValue.h>
#include <LibWeb/CSS/StyleValues/BackgroundSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/BasicShapeStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/ConicGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/ContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/EasingStyleValue.h>
#include <LibWeb/CSS/StyleValues/EdgeStyleValue.h>
#include <LibWeb/CSS/StyleValues/FilterValueListStyleValue.h>
#include <LibWeb/CSS/StyleValues/FlexStyleValue.h>
#include <LibWeb/CSS/StyleValues/FrequencyStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridAutoFlowStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTemplateAreaStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackPlacementStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackSizeListStyleValue.h>
#include <LibWeb/CSS/StyleValues/IdentifierStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/InheritStyleValue.h>
#include <LibWeb/CSS/StyleValues/InitialStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/LinearGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/MathDepthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RadialGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/RectStyleValue.h>
#include <LibWeb/CSS/StyleValues/ResolutionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RevertStyleValue.h>
#include <LibWeb/CSS/StyleValues/ScrollbarGutterStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShadowStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransitionStyleValue.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnsetStyleValue.h>

namespace Web::CSS {

StyleValue::StyleValue(Type type)
    : m_type(type)
{
}

AbstractImageStyleValue const& StyleValue::as_abstract_image() const
{
    VERIFY(is_abstract_image());
    return static_cast<AbstractImageStyleValue const&>(*this);
}

#define __ENUMERATE_STYLE_VALUE_TYPE(TitleCaseName, SnakeCaseName)          \
    TitleCaseName##StyleValue const& StyleValue::as_##SnakeCaseName() const \
    {                                                                       \
        VERIFY(is_##SnakeCaseName());                                       \
        return static_cast<TitleCaseName##StyleValue const&>(*this);        \
    }
ENUMERATE_STYLE_VALUE_TYPES
#undef __ENUMERATE_STYLE_VALUE_TYPE

ValueComparingNonnullRefPtr<StyleValue const> StyleValue::absolutized(CSSPixelRect const&, Length::FontMetrics const&, Length::FontMetrics const&) const
{
    return *this;
}

bool StyleValue::has_auto() const
{
    return is_identifier() && as_identifier().id() == ValueID::Auto;
}

ValueID StyleValue::to_identifier() const
{
    if (is_identifier())
        return as_identifier().id();
    return ValueID::Invalid;
}

int StyleValue::to_font_weight() const
{
    if (is_identifier()) {
        switch (static_cast<IdentifierStyleValue const&>(*this).id()) {
        case CSS::ValueID::Normal:
            return Gfx::FontWeight::Regular;
        case CSS::ValueID::Bold:
            return Gfx::FontWeight::Bold;
        case CSS::ValueID::Lighter:
            // FIXME: This should be relative to the parent.
            return Gfx::FontWeight::Regular;
        case CSS::ValueID::Bolder:
            // FIXME: This should be relative to the parent.
            return Gfx::FontWeight::Bold;
        default:
            return Gfx::FontWeight::Regular;
        }
    }
    if (is_number()) {
        return round_to<int>(as_number().number());
    }
    if (is_calculated()) {
        auto maybe_weight = const_cast<CalculatedStyleValue&>(as_calculated()).resolve_integer();
        if (maybe_weight.has_value())
            return maybe_weight.value();
    }
    return Gfx::FontWeight::Regular;
}

int StyleValue::to_font_slope() const
{
    // FIXME: Implement oblique <angle>
    if (is_identifier()) {
        switch (static_cast<IdentifierStyleValue const&>(*this).id()) {
        case CSS::ValueID::Italic: {
            static int italic_slope = Gfx::name_to_slope("Italic"sv);
            return italic_slope;
        }
        case CSS::ValueID::Oblique:
            static int oblique_slope = Gfx::name_to_slope("Oblique"sv);
            return oblique_slope;
        case CSS::ValueID::Normal:
        default:
            break;
        }
    }
    static int normal_slope = Gfx::name_to_slope("Normal"sv);
    return normal_slope;
}

int StyleValue::to_font_stretch_width() const
{
    int width = Gfx::FontWidth::Normal;
    if (is_identifier()) {
        switch (static_cast<IdentifierStyleValue const&>(*this).id()) {
        case CSS::ValueID::UltraCondensed:
            width = Gfx::FontWidth::UltraCondensed;
            break;
        case CSS::ValueID::ExtraCondensed:
            width = Gfx::FontWidth::ExtraCondensed;
            break;
        case CSS::ValueID::Condensed:
            width = Gfx::FontWidth::Condensed;
            break;
        case CSS::ValueID::SemiCondensed:
            width = Gfx::FontWidth::SemiCondensed;
            break;
        case CSS::ValueID::Normal:
            width = Gfx::FontWidth::Normal;
            break;
        case CSS::ValueID::SemiExpanded:
            width = Gfx::FontWidth::SemiExpanded;
            break;
        case CSS::ValueID::Expanded:
            width = Gfx::FontWidth::Expanded;
            break;
        case CSS::ValueID::ExtraExpanded:
            width = Gfx::FontWidth::ExtraExpanded;
            break;
        case CSS::ValueID::UltraExpanded:
            width = Gfx::FontWidth::UltraExpanded;
            break;
        default:
            break;
        }
    } else if (is_percentage()) {
        float percentage = as_percentage().percentage().value();
        if (percentage <= 50) {
            width = Gfx::FontWidth::UltraCondensed;
        } else if (percentage <= 62.5f) {
            width = Gfx::FontWidth::ExtraCondensed;
        } else if (percentage <= 75.0f) {
            width = Gfx::FontWidth::Condensed;
        } else if (percentage <= 87.5f) {
            width = Gfx::FontWidth::SemiCondensed;
        } else if (percentage <= 100.0f) {
            width = Gfx::FontWidth::Normal;
        } else if (percentage <= 112.5f) {
            width = Gfx::FontWidth::SemiExpanded;
        } else if (percentage <= 125.0f) {
            width = Gfx::FontWidth::Expanded;
        } else if (percentage <= 150.0f) {
            width = Gfx::FontWidth::ExtraExpanded;
        } else {
            width = Gfx::FontWidth::UltraExpanded;
        }
    }
    return width;
}

}
