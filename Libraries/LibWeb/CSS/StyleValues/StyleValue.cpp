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
#include <LibWeb/CSS/StyleValues/AnchorSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/AnchorStyleValue.h>
#include <LibWeb/CSS/StyleValues/AngleStyleValue.h>
#include <LibWeb/CSS/StyleValues/BackgroundSizeStyleValue.h>
#include <LibWeb/CSS/StyleValues/BasicShapeStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderImageSliceStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusRectStyleValue.h>
#include <LibWeb/CSS/StyleValues/BorderRadiusStyleValue.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorFunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorInterpolationMethodStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorMixStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorSchemeStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/ConicGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/ContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/ContrastColorStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterDefinitionsStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleSystemStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleValue.h>
#include <LibWeb/CSS/StyleValues/CursorStyleValue.h>
#include <LibWeb/CSS/StyleValues/CustomIdentStyleValue.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/EasingStyleValue.h>
#include <LibWeb/CSS/StyleValues/EdgeStyleValue.h>
#include <LibWeb/CSS/StyleValues/EmptyOptionalStyleValue.h>
#include <LibWeb/CSS/StyleValues/FilterStyleValue.h>
#include <LibWeb/CSS/StyleValues/FlexStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontSourceStyleValue.h>
#include <LibWeb/CSS/StyleValues/FontStyleStyleValue.h>
#include <LibWeb/CSS/StyleValues/FrequencyStyleValue.h>
#include <LibWeb/CSS/StyleValues/FunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridAutoFlowStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTemplateAreaStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackPlacementStyleValue.h>
#include <LibWeb/CSS/StyleValues/GridTrackSizeListStyleValue.h>
#include <LibWeb/CSS/StyleValues/GuaranteedInvalidStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageSetStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/LightDarkStyleValue.h>
#include <LibWeb/CSS/StyleValues/LinearGradientStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpacityValueStyleValue.h>
#include <LibWeb/CSS/StyleValues/OpenTypeTaggedStyleValue.h>
#include <LibWeb/CSS/StyleValues/OverflowClipMarginStyleValue.h>
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
#include <LibWeb/ComputedValuesRustFFI.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/Layout/Node.h>

extern "C" void ladybird_style_value_unref(void const*);
extern "C" void ladybird_utf16_fly_string_unref(size_t);
extern "C" void ladybird_style_value_ref(void const*);
extern "C" void ladybird_utf16_fly_string_ref(size_t);
extern "C" void ladybird_string_unref(size_t);

namespace Web::CSS {

ColorResolutionContext ColorResolutionContext::for_element(DOM::AbstractElement const& element)
{
    auto computed_values = element.computed_values();
    VERIFY(computed_values);

    CalculationResolutionContext calculation_resolution_context { .length_resolution_context = Length::ResolutionContext::for_element(element) };

    return {
        .color_scheme = computed_values->color_scheme(),
        .current_color = computed_values->color(),
        .current_color_style_value = computed_values->color_style_value(),
        .calculation_resolution_context = calculation_resolution_context
    };
}

ColorResolutionContext ColorResolutionContext::for_layout_node_with_style(Layout::NodeWithStyle const& layout_node)
{
    RefPtr<StyleValue const> current_color_style_value;
    if (auto* dom_node = layout_node.dom_node()) {
        if (auto* element = as_if<DOM::Element>(*dom_node)) {
            if (auto computed_values = element->computed_values())
                current_color_style_value = computed_values->color_style_value();
        }
    }

    return {
        .color_scheme = layout_node.computed_values().color_scheme(),
        .current_color = layout_node.computed_values().color(),
        .current_color_style_value = current_color_style_value,
        .calculation_resolution_context = { .length_resolution_context = Length::ResolutionContext::for_layout_node(layout_node) },
    };
}

StyleValue::StyleValue(Type type, StyleValueFFI::StyleValueData* value)
    : m_value(value)
    , m_type(type)
{
}

void StyleValue::set_style_sheet(GC::Ptr<CSSStyleSheet> style_sheet)
{
    // Only the types holding nested values with document-associated state care.
    switch (type()) {
    case Type::Content:
        return as_content().set_style_sheet(style_sheet);
    case Type::Image:
        return as_image().set_style_sheet(style_sheet);
    case Type::ImageSet:
        return as_image_set().set_style_sheet(style_sheet);
    case Type::Shorthand:
        return as_shorthand().set_style_sheet(style_sheet);
    case Type::ValueList:
        return as_value_list().set_style_sheet(style_sheet);
    default:
        return;
    }
}

bool StyleValue::is_computationally_independent() const
{
    // The rules for the value types whose decision has moved into the Rust style computation
    // core; the rest still dispatch to their C++ shells.
    auto decision = ComputedValuesFFI::rust_style_value_is_computationally_independent(
        m_value.operator->(),
        [](void const* shell) -> void const* { return static_cast<StyleValue const*>(shell)->rust_style_value_data(); },
        [](void const* shell) -> bool { return static_cast<StyleValue const*>(shell)->decide_computational_independence_fallback(); });
    if (decision.handled)
        return decision.independent;

    // The only value types deciding here are the grid track size list, which delegates to a
    // deliberately C++-backed value type, and calc values, whose calculation trees are opaque
    // to the core. The unresolved, guaranteed-invalid, pending-substitution, unicode-range and
    // counter-style-system values must never be asked, which the default preserves.
    return decide_computational_independence_fallback();
}

bool StyleValue::decide_computational_independence_fallback() const
{
    switch (type()) {
    case Type::GridTrackSizeList:
        return static_cast<GridTrackSizeListStyleValue const&>(*this).is_computationally_independent();
    default:
        VERIFY_NOT_REACHED();
    }
}

void StyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    switch (type()) {
#define __ENUMERATE_CSS_STYLE_VALUE_TYPE(title_case, snake_case, style_value_class_name) \
    case Type::title_case:                                                               \
        return static_cast<style_value_class_name const&>(*this).serialize(builder, mode);
        ENUMERATE_CSS_STYLE_VALUE_TYPES
#undef __ENUMERATE_CSS_STYLE_VALUE_TYPE
    }
    VERIFY_NOT_REACHED();
}

bool StyleValue::equals(StyleValue const& other) const
{
    switch (type()) {
#define __ENUMERATE_CSS_STYLE_VALUE_TYPE(title_case, snake_case, style_value_class_name) \
    case Type::title_case:                                                               \
        return static_cast<style_value_class_name const&>(*this).equals(other);
        ENUMERATE_CSS_STYLE_VALUE_TYPES
#undef __ENUMERATE_CSS_STYLE_VALUE_TYPE
    }
    VERIFY_NOT_REACHED();
}

bool StyleValue::is_color_function() const
{
    return type() == Type::Color && m_value->tag == StyleValueFFI::StyleValueData::Tag::ColorFunction;
}

bool StyleValue::depends_on_current_color() const
{
    // The recursion over nested colors lives in the Rust style value graph.
    return StyleValueFFI::rust_style_value_depends_on_current_color(
        m_value.operator->(),
        [](void const* shell) -> void const* { return static_cast<StyleValue const*>(shell)->rust_style_value_data(); });
}

bool StyleValue::has_color() const
{
    if (type() == Type::Color)
        return true;
    if (type() == Type::Keyword)
        return as_keyword().has_color();
    return false;
}

Optional<Color> StyleValue::to_color(ColorResolutionContext color_resolution_context) const
{
    if (type() == Type::Color)
        return as_color().to_color(color_resolution_context);
    if (type() == Type::Keyword)
        return as_keyword().to_color(color_resolution_context);
    return {};
}

String StyleValue::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    serialize(builder, mode);
    return builder.to_string_without_validation();
}

Utf16String StyleValue::to_utf16_string(SerializationMode mode) const
{
    Utf16StringBuilder builder;
    serialize(builder, mode);
    return builder.to_string();
}

void StyleValue::serialize(Utf16StringBuilder& builder, SerializationMode mode) const
{
    switch (type()) {
    case Type::Easing:
        return as_easing().serialize(builder, mode);
    case Type::Integer:
        return as_integer().serialize(builder, mode);
    case Type::Keyword:
        return as_keyword().serialize(builder, mode);
    case Type::Length:
        return as_length().serialize(builder, mode);
    case Type::Ratio:
        return as_ratio().serialize(builder, mode);
    case Type::Resolution:
        return as_resolution().serialize(builder, mode);
    default:
        break;
    }
    auto serialized = to_string(mode);
    auto serialized_utf16 = Utf16String::from_utf8_without_validation(serialized);
    builder.append(serialized_utf16.utf16_view());
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

ValueComparingNonnullRefPtr<StyleValue const> StyleValue::absolutized(ComputationContext const& context) const
{
    switch (type()) {
    case Type::Angle:
        return as_angle().absolutized(context);
    case Type::BackgroundSize:
        return as_background_size().absolutized(context);
    case Type::BasicShape:
        return as_basic_shape().absolutized(context);
    case Type::BorderRadiusRect:
        return as_border_radius_rect().absolutized(context);
    case Type::BorderRadius:
        return as_border_radius().absolutized(context);
    case Type::Calculated:
        return as_calculated().absolutized(context);
    case Type::Color:
        return as_color().absolutized(context);
    case Type::ConicGradient:
        return as_conic_gradient().absolutized(context);
    case Type::CounterDefinitions:
        return as_counter_definitions().absolutized(context);
    case Type::CounterStyleSystem:
        return as_counter_style_system().absolutized(context);
    case Type::Edge:
        return as_edge().absolutized(context);
    case Type::FontStyle:
        return as_font_style().absolutized(context);
    case Type::Position:
        return as_position().absolutized(context);
    case Type::Cursor:
        return as_cursor().absolutized(context);
    case Type::Easing:
        return as_easing().absolutized(context);
    case Type::Filter:
        return as_filter().absolutized(context);
    case Type::Frequency:
        return as_frequency().absolutized(context);
    case Type::Function:
        return as_function().absolutized(context);
    case Type::GridTrackPlacement:
        return as_grid_track_placement().absolutized(context);
    case Type::GridTrackSizeList:
        return as_grid_track_size_list().absolutized(context);
    case Type::ImageSet:
        return as_image_set().absolutized(context);
    case Type::Image:
        return as_image().absolutized(context);
    case Type::Keyword:
        return as_keyword().absolutized(context);
    case Type::Length:
        return as_length().absolutized(context);
    case Type::LinearGradient:
        return as_linear_gradient().absolutized(context);
    case Type::OpacityValue:
        return as_opacity_value().absolutized(context);
    case Type::OpenTypeTagged:
        return as_open_type_tagged().absolutized(context);
    case Type::OverflowClipMargin:
        return as_overflow_clip_margin().absolutized(context);
    case Type::RadialGradient:
        return as_radial_gradient().absolutized(context);
    case Type::RadialSize:
        return as_radial_size().absolutized(context);
    case Type::RandomValueSharing:
        return as_random_value_sharing().absolutized(context);
    case Type::Ratio:
        return as_ratio().absolutized(context);
    case Type::Rect:
        return as_rect().absolutized(context);
    case Type::Resolution:
        return as_resolution().absolutized(context);
    case Type::ScrollbarColor:
        return as_scrollbar_color().absolutized(context);
    case Type::Shadow:
        return as_shadow().absolutized(context);
    case Type::Superellipse:
        return as_superellipse().absolutized(context);
    case Type::TextIndent:
        return as_text_indent().absolutized(context);
    case Type::Time:
        return as_time().absolutized(context);
    case Type::Transformation:
        return as_transformation().absolutized(context);
    case Type::TreeCountingFunction:
        return as_tree_counting_function().absolutized(context);
    case Type::Tuple:
        return as_tuple().absolutized(context);
    case Type::ValueList:
        return as_value_list().absolutized(context);
    default:
        return *this;
    }
}

bool StyleValue::has_auto() const
{
    return is_keyword() && as_keyword().keyword() == Keyword::Auto;
}

Vector<Parser::ComponentValue> StyleValue::tokenize() const
{
    switch (type()) {
    case Type::Angle:
    case Type::Flex:
    case Type::Frequency:
    case Type::Length:
    case Type::Percentage:
    case Type::Resolution:
    case Type::Time:
        return as_dimension().tokenize();
    case Type::CustomIdent:
        return as_custom_ident().tokenize();
    case Type::EmptyOptional:
        return as_empty_optional().tokenize();
    case Type::GuaranteedInvalid:
        return as_guaranteed_invalid().tokenize();
    case Type::Integer:
        return as_integer().tokenize();
    case Type::Keyword:
        return as_keyword().tokenize();
    case Type::Number:
        return as_number().tokenize();
    case Type::PendingSubstitution:
        return as_pending_substitution().tokenize();
    case Type::Ratio:
        return as_ratio().tokenize();
    case Type::String:
        return as_string().tokenize();
    case Type::Unresolved:
        return as_unresolved().tokenize();
    case Type::ValueList:
        return as_value_list().tokenize();
    default:
        break;
    }
    // This is an inefficient way of producing ComponentValues, but it's guaranteed to work for types that round-trip.
    // FIXME: Implement better versions in the subclasses.
    return Parser::Parser::create(Parser::ParsingParams {}, to_string(SerializationMode::ResolvedValue)).parse_as_list_of_component_values();
}

// https://drafts.css-houdini.org/css-typed-om-1/#reify-as-a-cssstylevalue
GC::Ref<CSSStyleValue> StyleValue::reify(JS::Realm& realm, Utf16FlyString const& associated_property) const
{
    switch (type()) {
    case Type::ConicGradient:
    case Type::Image:
    case Type::ImageSet:
    case Type::LinearGradient:
    case Type::RadialGradient:
        return as_abstract_image().reify(realm, associated_property);
    case Type::Angle:
    case Type::Flex:
    case Type::Frequency:
    case Type::Length:
    case Type::Percentage:
    case Type::Resolution:
    case Type::Time:
        return as_dimension().reify(realm, associated_property);
    case Type::Calculated:
        return as_calculated().reify(realm, associated_property);
    case Type::CustomIdent:
        return as_custom_ident().reify(realm, associated_property);
    case Type::Display:
        return as_display().reify(realm, associated_property);
    case Type::Integer:
        return as_integer().reify(realm, associated_property);
    case Type::Keyword:
        return as_keyword().reify(realm, associated_property);
    case Type::Number:
        return as_number().reify(realm, associated_property);
    case Type::OpacityValue:
        return as_opacity_value().reify(realm, associated_property);
    case Type::Unresolved:
        return as_unresolved().reify(realm, associated_property);
    case Type::ValueList:
        return as_value_list().reify(realm, associated_property);
    default:
        break;
    }
    return default_reify(realm, associated_property);
}

GC::Ref<CSSStyleValue> StyleValue::default_reify(JS::Realm& realm, Utf16FlyString const& associated_property) const
{
    // 1. Return a new CSSStyleValue object representing value whose [[associatedProperty]] internal slot is set to property.
    return CSSStyleValue::create(realm, associated_property, *this);
}

// https://drafts.css-houdini.org/css-typed-om-1/#subdivide-into-iterations
StyleValueVector StyleValue::subdivide_into_iterations(PropertyNameAndID const& property) const
{
    // To subdivide into iterations a CSS value whole value for a property property, execute the following steps:
    // 1. If property is a single-valued property, return a list containing whole value.
    // 2. Otherwise, divide whole value into individual iterations, as appropriate for property, and return a list
    //    containing the iterations in order.
    // NB: We do this by type. By default, we assume step 1 applies. Step 2 applies to value lists.
    if (type() == Type::ValueList)
        return as_value_list().subdivide_into_iterations(property);
    return StyleValueVector { *this };
}

i32 int_from_style_value(NonnullRefPtr<StyleValue const> const& style_value)
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

Utf16FlyString string_from_style_value(NonnullRefPtr<StyleValue const> const& style_value)
{
    if (style_value->is_string())
        return style_value->as_string().string_value();

    if (style_value->is_custom_ident())
        return style_value->as_custom_ident().custom_ident();

    VERIFY_NOT_REACHED();
}

Keyword StyleValue::to_keyword() const
{
    if (is_keyword())
        return static_cast<KeywordStyleValue const&>(*this).keyword();
    return Keyword::Invalid;
}

}

// Called when Rust-owned style value data drops a reference to a C++ style value it retained.
extern "C" void ladybird_style_value_unref(void const* style_value)
{
    static_cast<Web::CSS::StyleValue const*>(style_value)->unref();
}

// Called when Rust-owned style value data drops a retained Utf16FlyString.
extern "C" void ladybird_utf16_fly_string_unref(size_t raw)
{
    Utf16FlyString::unref_raw(raw);
}

// Called when Rust-owned cascade data retains an additional reference to a C++ style value.
extern "C" void ladybird_style_value_ref(void const* style_value)
{
    static_cast<Web::CSS::StyleValue const*>(style_value)->ref();
}

// Called when Rust-owned cascade data retains an additional reference to a Utf16FlyString.
extern "C" void ladybird_utf16_fly_string_ref(size_t raw)
{
    (void)Utf16FlyString::from_raw(raw).to_raw_leaked();
}

// Called when Rust-owned style value data drops a retained String.
extern "C" void ladybird_string_unref(size_t raw)
{
    String::unref_raw(raw);
}
