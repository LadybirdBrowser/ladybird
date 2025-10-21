/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontStyleMapping.h>
#include <LibGfx/Font/FontWeight.h>
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/AnchorSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/AnchorStyleValue.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/BackgroundSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/BasicShapeStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderImageSliceStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorSchemeStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/ConicGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/ContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterDefinitionsStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleValue.h>
#include <LibWeb/CSS/StyleValues/CursorStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/EasingStyleValue.h>
#include <LibWeb/CSS/StyleValues/EdgeStyleValue.h>
#include <LibWeb/CSS/StyleValues/FilterValueListStyleValue.h>
#include <LibWeb/CSS/StyleValues/FitContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/FlexStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontSourceStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/FrequencyStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridAutoFlowStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTemplateAreaStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackPlacementStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackSizeListStyleValue.h>
#include <LibWeb/CSS/StyleValues/GuaranteedInvalidStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/LinearGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/MathDepthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpenTypeTaggedStyleValue.h>
#include <LibWeb/CSS/StyleValues/PendingSubstitutionStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RadialGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/RectStyleValue.h>
#include <LibWeb/CSS/StyleValues/RepeatStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/ResolutionStyleValue.h>
#include <LibWeb/CSS/StyleValues/ScrollbarColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/ScrollbarGutterStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShadowStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/SuperellipseStyleValue.h>
#include <LibWeb/CSS/StyleValues/TextUnderlinePositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransitionStyleValue.h>
#include <LibWeb/CSS/StyleValues/TreeCountingFunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnicodeRangeStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/Layout/Node.h>

namespace Web::CSS {

ColorResolutionContext ColorResolutionContext::for_element(DOM::AbstractElement const& element)
{
    auto color_scheme = element.computed_properties()->color_scheme(element.document().page().preferred_color_scheme(), element.document().supported_color_schemes());

    CalculationResolutionContext calculation_resolution_context { .length_resolution_context = Length::ResolutionContext::for_element(element) };

    return {
        .color_scheme = color_scheme,
        .current_color = element.computed_properties()->color_or_fallback(PropertyID::Color, { color_scheme, CSS::InitialValues::color(), element.document(), calculation_resolution_context }, CSS::InitialValues::color()),
        .document = element.document(),
        .calculation_resolution_context = calculation_resolution_context
    };
}

ColorResolutionContext ColorResolutionContext::for_layout_node_with_style(Layout::NodeWithStyle const& layout_node)
{
    return {
        .color_scheme = layout_node.computed_values().color_scheme(),
        .current_color = layout_node.computed_values().color(),
        .document = layout_node.document(),
        .calculation_resolution_context = { .length_resolution_context = Length::ResolutionContext::for_layout_node(layout_node) },
    };
}

StyleValue::StyleValue(Type type)
    : m_type(type)
{
}

AbstractImageStyleValue const& StyleValue::as_abstract_image() const
{
    VERIFY(is_abstract_image());
    return static_cast<AbstractImageStyleValue const&>(*this);
}

DimensionStyleValue const& StyleValue::as_dimension() const
{
    VERIFY(is_dimension());
    return static_cast<DimensionStyleValue const&>(*this);
}

#define __ENUMERATE_CSS_STYLE_VALUE_TYPE(title_case, snake_case, style_value_class_name) \
    style_value_class_name const& StyleValue::as_##snake_case() const                    \
    {                                                                                    \
        VERIFY(is_##snake_case());                                                       \
        return static_cast<style_value_class_name const&>(*this);                        \
    }
ENUMERATE_CSS_STYLE_VALUE_TYPES
#undef __ENUMERATE_CSS_STYLE_VALUE_TYPE

ValueComparingNonnullRefPtr<StyleValue const> StyleValue::absolutized(ComputationContext const&) const
{
    return *this;
}

bool StyleValue::has_auto() const
{
    return is_keyword() && as_keyword().keyword() == Keyword::Auto;
}

Vector<Parser::ComponentValue> StyleValue::tokenize() const
{
    // This is an inefficient way of producing ComponentValues, but it's guaranteed to work for types that round-trip.
    // FIXME: Implement better versions in the subclasses.
    return Parser::Parser::create(Parser::ParsingParams {}, to_string(SerializationMode::Normal)).parse_as_list_of_component_values();
}

// https://drafts.css-houdini.org/css-typed-om-1/#reify-as-a-cssstylevalue
GC::Ref<CSSStyleValue> StyleValue::reify(JS::Realm& realm, FlyString const& associated_property) const
{
    // 1. Return a new CSSStyleValue object representing value whose [[associatedProperty]] internal slot is set to property.
    return CSSStyleValue::create(realm, associated_property, *this);
}

}
