/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Path.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGLineElement.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGLineElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGLineElement);

SVGLineElement::SVGLineElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGeometryElement(document, qualified_name)
{
}

void SVGLineElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGLineElement);
    Base::initialize(realm);
}

bool SVGLineElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return first_is_one_of(name,
        SVG::AttributeNames::x1,
        SVG::AttributeNames::y1,
        SVG::AttributeNames::x2,
        SVG::AttributeNames::y2);
}

void SVGLineElement::apply_presentational_hints(Vector<CSS::StyleProperty>& properties) const
{
    Base::apply_presentational_hints(properties);
    auto parsing_context = CSS::Parser::ParsingParams { document(), CSS::Parser::ParsingMode::SVGPresentationAttribute };

    auto x1_attribute = attribute(SVG::AttributeNames::x1);
    if (auto x1_value = parse_css_value(parsing_context, x1_attribute.value_or(String {}), CSS::PropertyID::X1))
        properties.append({ .property_id = CSS::PropertyID::X1, .value = x1_value.release_nonnull() });

    auto y1_attribute = attribute(SVG::AttributeNames::y1);
    if (auto y1_value = parse_css_value(parsing_context, y1_attribute.value_or(String {}), CSS::PropertyID::Y1))
        properties.append({ .property_id = CSS::PropertyID::Y1, .value = y1_value.release_nonnull() });

    auto x2_attribute = attribute(SVG::AttributeNames::x2);
    if (auto x2_value = parse_css_value(parsing_context, x2_attribute.value_or(String {}), CSS::PropertyID::X2))
        properties.append({ .property_id = CSS::PropertyID::X2, .value = x2_value.release_nonnull() });

    auto y2_attribute = attribute(SVG::AttributeNames::y2);
    if (auto y2_value = parse_css_value(parsing_context, y2_attribute.value_or(String {}), CSS::PropertyID::Y2))
        properties.append({ .property_id = CSS::PropertyID::Y2, .value = y2_value.release_nonnull() });
}

Gfx::Path SVGLineElement::get_path(CSSPixelSize viewport_size)
{
    auto node = unsafe_layout_node();
    if (!node) {
        dbgln("FIXME: Null layout node in SVGLineElement::get_path");
        return {};
    }

    Gfx::Path path;
    float const x1 = node->computed_values().x1().to_px(*node, viewport_size.width()).to_float();
    float const y1 = node->computed_values().y1().to_px(*node, viewport_size.height()).to_float();
    float const x2 = node->computed_values().x2().to_px(*node, viewport_size.width()).to_float();
    float const y2 = node->computed_values().y2().to_px(*node, viewport_size.height()).to_float();

    // 1. perform an absolute moveto operation to absolute location (x1,y1)
    path.move_to({ x1, y1 });

    // 2. perform an absolute lineto operation to absolute location (x2,y2)
    path.line_to({ x2, y2 });

    return path;
}

// https://www.w3.org/TR/SVG11/shapes.html#LineElementX1Attribute
GC::Ref<SVGAnimatedLength> SVGLineElement::x1() const
{
    return svg_animated_length_for_property(CSS::PropertyID::X1);
}

// https://www.w3.org/TR/SVG11/shapes.html#LineElementY1Attribute
GC::Ref<SVGAnimatedLength> SVGLineElement::y1() const
{
    return svg_animated_length_for_property(CSS::PropertyID::Y1);
}

// https://www.w3.org/TR/SVG11/shapes.html#LineElementX2Attribute
GC::Ref<SVGAnimatedLength> SVGLineElement::x2() const
{
    return svg_animated_length_for_property(CSS::PropertyID::X2);
}

// https://www.w3.org/TR/SVG11/shapes.html#LineElementY2Attribute
GC::Ref<SVGAnimatedLength> SVGLineElement::y2() const
{
    return svg_animated_length_for_property(CSS::PropertyID::Y2);
}

}
