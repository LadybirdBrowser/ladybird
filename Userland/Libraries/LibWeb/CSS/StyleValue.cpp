/*
 * Copyright (c) 2018-2023, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
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
#include <LibWeb/CSS/StyleValues/CounterDefinitionsStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleValue.h>
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

AngleStyleValue const& StyleValue::as_angle() const
{
    VERIFY(is_angle());
    return static_cast<AngleStyleValue const&>(*this);
}

BackgroundRepeatStyleValue const& StyleValue::as_background_repeat() const
{
    VERIFY(is_background_repeat());
    return static_cast<BackgroundRepeatStyleValue const&>(*this);
}

BackgroundSizeStyleValue const& StyleValue::as_background_size() const
{
    VERIFY(is_background_size());
    return static_cast<BackgroundSizeStyleValue const&>(*this);
}

BasicShapeStyleValue const& StyleValue::as_basic_shape() const
{
    VERIFY(is_basic_shape());
    return static_cast<BasicShapeStyleValue const&>(*this);
}

BorderRadiusStyleValue const& StyleValue::as_border_radius() const
{
    VERIFY(is_border_radius());
    return static_cast<BorderRadiusStyleValue const&>(*this);
}

CalculatedStyleValue const& StyleValue::as_calculated() const
{
    VERIFY(is_calculated());
    return static_cast<CalculatedStyleValue const&>(*this);
}

ColorStyleValue const& StyleValue::as_color() const
{
    VERIFY(is_color());
    return static_cast<ColorStyleValue const&>(*this);
}

ConicGradientStyleValue const& StyleValue::as_conic_gradient() const
{
    VERIFY(is_conic_gradient());
    return static_cast<ConicGradientStyleValue const&>(*this);
}

ContentStyleValue const& StyleValue::as_content() const
{
    VERIFY(is_content());
    return static_cast<ContentStyleValue const&>(*this);
}

CounterStyleValue const& StyleValue::as_counter() const
{
    VERIFY(is_counter());
    return static_cast<CounterStyleValue const&>(*this);
}

CounterDefinitionsStyleValue const& StyleValue::as_counter_definitions() const
{
    VERIFY(is_counter_definitions());
    return static_cast<CounterDefinitionsStyleValue const&>(*this);
}

CustomIdentStyleValue const& StyleValue::as_custom_ident() const
{
    VERIFY(is_custom_ident());
    return static_cast<CustomIdentStyleValue const&>(*this);
}

DisplayStyleValue const& StyleValue::as_display() const
{
    VERIFY(is_display());
    return static_cast<DisplayStyleValue const&>(*this);
}

EasingStyleValue const& StyleValue::as_easing() const
{
    VERIFY(is_easing());
    return static_cast<EasingStyleValue const&>(*this);
}

EdgeStyleValue const& StyleValue::as_edge() const
{
    VERIFY(is_edge());
    return static_cast<EdgeStyleValue const&>(*this);
}

FilterValueListStyleValue const& StyleValue::as_filter_value_list() const
{
    VERIFY(is_filter_value_list());
    return static_cast<FilterValueListStyleValue const&>(*this);
}

FlexStyleValue const& StyleValue::as_flex() const
{
    VERIFY(is_flex());
    return static_cast<FlexStyleValue const&>(*this);
}

FrequencyStyleValue const& StyleValue::as_frequency() const
{
    VERIFY(is_frequency());
    return static_cast<FrequencyStyleValue const&>(*this);
}

GridAutoFlowStyleValue const& StyleValue::as_grid_auto_flow() const
{
    VERIFY(is_grid_auto_flow());
    return static_cast<GridAutoFlowStyleValue const&>(*this);
}

GridTemplateAreaStyleValue const& StyleValue::as_grid_template_area() const
{
    VERIFY(is_grid_template_area());
    return static_cast<GridTemplateAreaStyleValue const&>(*this);
}

GridTrackPlacementStyleValue const& StyleValue::as_grid_track_placement() const
{
    VERIFY(is_grid_track_placement());
    return static_cast<GridTrackPlacementStyleValue const&>(*this);
}

GridTrackSizeListStyleValue const& StyleValue::as_grid_track_size_list() const
{
    VERIFY(is_grid_track_size_list());
    return static_cast<GridTrackSizeListStyleValue const&>(*this);
}

IdentifierStyleValue const& StyleValue::as_identifier() const
{
    VERIFY(is_identifier());
    return static_cast<IdentifierStyleValue const&>(*this);
}

ImageStyleValue const& StyleValue::as_image() const
{
    VERIFY(is_image());
    return static_cast<ImageStyleValue const&>(*this);
}

InheritStyleValue const& StyleValue::as_inherit() const
{
    VERIFY(is_inherit());
    return static_cast<InheritStyleValue const&>(*this);
}

InitialStyleValue const& StyleValue::as_initial() const
{
    VERIFY(is_initial());
    return static_cast<InitialStyleValue const&>(*this);
}

IntegerStyleValue const& StyleValue::as_integer() const
{
    VERIFY(is_integer());
    return static_cast<IntegerStyleValue const&>(*this);
}

LengthStyleValue const& StyleValue::as_length() const
{
    VERIFY(is_length());
    return static_cast<LengthStyleValue const&>(*this);
}

LinearGradientStyleValue const& StyleValue::as_linear_gradient() const
{
    VERIFY(is_linear_gradient());
    return static_cast<LinearGradientStyleValue const&>(*this);
}

MathDepthStyleValue const& StyleValue::as_math_depth() const
{
    VERIFY(is_math_depth());
    return static_cast<MathDepthStyleValue const&>(*this);
}

NumberStyleValue const& StyleValue::as_number() const
{
    VERIFY(is_number());
    return static_cast<NumberStyleValue const&>(*this);
}

PercentageStyleValue const& StyleValue::as_percentage() const
{
    VERIFY(is_percentage());
    return static_cast<PercentageStyleValue const&>(*this);
}

PositionStyleValue const& StyleValue::as_position() const
{
    VERIFY(is_position());
    return static_cast<PositionStyleValue const&>(*this);
}

RadialGradientStyleValue const& StyleValue::as_radial_gradient() const
{
    VERIFY(is_radial_gradient());
    return static_cast<RadialGradientStyleValue const&>(*this);
}

RatioStyleValue const& StyleValue::as_ratio() const
{
    VERIFY(is_ratio());
    return static_cast<RatioStyleValue const&>(*this);
}

RectStyleValue const& StyleValue::as_rect() const
{
    VERIFY(is_rect());
    return static_cast<RectStyleValue const&>(*this);
}

ResolutionStyleValue const& StyleValue::as_resolution() const
{
    VERIFY(is_resolution());
    return static_cast<ResolutionStyleValue const&>(*this);
}

RevertStyleValue const& StyleValue::as_revert() const
{
    VERIFY(is_revert());
    return static_cast<RevertStyleValue const&>(*this);
}

ScrollbarGutterStyleValue const& StyleValue::as_scrollbar_gutter() const
{
    VERIFY(is_scrollbar_gutter());
    return static_cast<ScrollbarGutterStyleValue const&>(*this);
}

ShadowStyleValue const& StyleValue::as_shadow() const
{
    VERIFY(is_shadow());
    return static_cast<ShadowStyleValue const&>(*this);
}

ShorthandStyleValue const& StyleValue::as_shorthand() const
{
    VERIFY(is_shorthand());
    return static_cast<ShorthandStyleValue const&>(*this);
}

StringStyleValue const& StyleValue::as_string() const
{
    VERIFY(is_string());
    return static_cast<StringStyleValue const&>(*this);
}

TimeStyleValue const& StyleValue::as_time() const
{
    VERIFY(is_time());
    return static_cast<TimeStyleValue const&>(*this);
}

TransformationStyleValue const& StyleValue::as_transformation() const
{
    VERIFY(is_transformation());
    return static_cast<TransformationStyleValue const&>(*this);
}

TransitionStyleValue const& StyleValue::as_transition() const
{
    VERIFY(is_transition());
    return static_cast<TransitionStyleValue const&>(*this);
}

UnresolvedStyleValue const& StyleValue::as_unresolved() const
{
    VERIFY(is_unresolved());
    return static_cast<UnresolvedStyleValue const&>(*this);
}

UnsetStyleValue const& StyleValue::as_unset() const
{
    VERIFY(is_unset());
    return static_cast<UnsetStyleValue const&>(*this);
}

URLStyleValue const& StyleValue::as_url() const
{
    VERIFY(is_url());
    return static_cast<URLStyleValue const&>(*this);
}

StyleValueList const& StyleValue::as_value_list() const
{
    VERIFY(is_value_list());
    return static_cast<StyleValueList const&>(*this);
}

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
