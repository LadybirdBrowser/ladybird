/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/FontStyleMapping.h>
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/AddFunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/AnchorSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/AnchorStyleValue.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/BackgroundSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/BasicShapeStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderImageSliceStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusRectStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorSchemeStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/ConicGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/ContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterDefinitionsStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleSystemStyleValue.h>
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
#include <LibWeb/CSS/StyleValues/FontVariantAlternatesFunctionStyleValue.h>
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
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpenTypeTaggedStyleValue.h>
#include <LibWeb/CSS/StyleValues/PendingSubstitutionStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/CSS/StyleValues/PositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RadialGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/RadialSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/RandomValueSharingStyleValue.h>
#include <LibWeb/CSS/StyleValues/RatioStyleValue.h>
#include <LibWeb/CSS/StyleValues/RectStyleValue.h>
#include <LibWeb/CSS/StyleValues/RepeatStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/ResolutionStyleValue.h>
#include <LibWeb/CSS/StyleValues/ScrollFunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/ScrollbarColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/ScrollbarGutterStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShadowStyleValue.h>
#include <LibWeb/CSS/StyleValues/ShorthandStyleValue.h>
#include <LibWeb/CSS/StyleValues/StringStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/SuperellipseStyleValue.h>
#include <LibWeb/CSS/StyleValues/TextIndentStyleValue.h>
#include <LibWeb/CSS/StyleValues/TextUnderlinePositionStyleValue.h>
#include <LibWeb/CSS/StyleValues/TimeStyleValue.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/CSS/StyleValues/TreeCountingFunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/TupleStyleValue.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnicodeRangeStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
#include <LibWeb/CSS/StyleValues/ViewFunctionStyleValue.h>
#include <LibWeb/CSS/SystemColor.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/Layout/Node.h>

namespace Web::CSS {

ColorResolutionContext ColorResolutionContext::for_element(DOM::AbstractElement const& element)
{
    auto color_scheme = element.computed_properties()->color_scheme(element.document().page().preferred_color_scheme(), element.document().supported_color_schemes());

    CalculationResolutionContext calculation_resolution_context { .length_resolution_context = Length::ResolutionContext::for_element(element) };

    return {
        .color_scheme = color_scheme,
        .current_color = element.computed_properties()->color_or_fallback(PropertyID::Color, { color_scheme, CSS::InitialValues::color(), CSS::SystemColor::accent_color(color_scheme), element.document(), calculation_resolution_context }, CSS::InitialValues::color()),
        .accent_color = element.computed_properties()->color_or_fallback(PropertyID::AccentColor, { color_scheme, CSS::InitialValues::color(), CSS::SystemColor::accent_color(color_scheme), element.document(), calculation_resolution_context }, CSS::SystemColor::accent_color(color_scheme)),
        .document = element.document(),
        .calculation_resolution_context = calculation_resolution_context
    };
}

ColorResolutionContext ColorResolutionContext::for_layout_node_with_style(Layout::NodeWithStyle const& layout_node)
{
    return {
        .color_scheme = layout_node.computed_values().color_scheme(),
        .current_color = layout_node.computed_values().color(),
        .accent_color = layout_node.computed_values().accent_color(),
        .document = layout_node.document(),
        .calculation_resolution_context = { .length_resolution_context = Length::ResolutionContext::for_layout_node(layout_node) },
    };
}

StyleValue::StyleValue(Type type)
    : m_type(type)
{
}

String StyleValue::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    serialize(builder, mode);
    return builder.to_string_without_validation();
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

// https://drafts.css-houdini.org/css-typed-om-1/#subdivide-into-iterations
StyleValueVector StyleValue::subdivide_into_iterations(PropertyNameAndID const&) const
{
    // To subdivide into iterations a CSS value whole value for a property property, execute the following steps:
    // 1. If property is a single-valued property, return a list containing whole value.
    // 2. Otherwise, divide whole value into individual iterations, as appropriate for property, and return a list
    //    containing the iterations in order.
    // NB: We do this by type. By default, we assume step 1 applies. For step 2, override this method.
    return StyleValueVector { *this };
}

i64 int_from_style_value(NonnullRefPtr<StyleValue const> const& style_value)
{
    if (style_value->is_integer())
        return style_value->as_integer().integer();

    if (style_value->is_calculated())
        return style_value->as_calculated().resolve_integer({}).value();

    VERIFY_NOT_REACHED();
}

double number_from_style_value(NonnullRefPtr<StyleValue const> const& style_value, Optional<double> percentage_basis)
{
    if (style_value->is_number())
        return style_value->as_number().number();

    if (style_value->is_calculated()) {
        auto const& calculated_style_value = style_value->as_calculated();

        if (calculated_style_value.resolves_to_number())
            return calculated_style_value.resolve_number({}).value();

        if (calculated_style_value.resolves_to_percentage()) {
            VERIFY(percentage_basis.has_value());

            return calculated_style_value.resolve_percentage({}).value().as_fraction() * percentage_basis.value();
        }

        VERIFY_NOT_REACHED();
    }

    if (style_value->is_percentage()) {
        VERIFY(percentage_basis.has_value());

        return percentage_basis.value() * style_value->as_percentage().percentage().as_fraction();
    }

    VERIFY_NOT_REACHED();
}

FlyString const& string_from_style_value(NonnullRefPtr<StyleValue const> const& style_value)
{
    if (style_value->is_string())
        return style_value->as_string().string_value();

    if (style_value->is_custom_ident())
        return style_value->as_custom_ident().custom_ident();

    VERIFY_NOT_REACHED();
}

}
