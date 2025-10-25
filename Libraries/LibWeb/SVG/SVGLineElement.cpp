/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Path.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGLineElementPrototype.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGLineElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGLineElement);

SVGLineElement::SVGLineElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGeometryElement(document, move(qualified_name))
{
}

void SVGLineElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGLineElement);
    Base::initialize(realm);
}

void SVGLineElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == SVG::AttributeNames::x1) {
        m_x1 = AttributeParser::parse_number_percentage(value.value_or(String {}));
    } else if (name == SVG::AttributeNames::y1) {
        m_y1 = AttributeParser::parse_number_percentage(value.value_or(String {}));
    } else if (name == SVG::AttributeNames::x2) {
        m_x2 = AttributeParser::parse_number_percentage(value.value_or(String {}));
    } else if (name == SVG::AttributeNames::y2) {
        m_y2 = AttributeParser::parse_number_percentage(value.value_or(String {}));
    }
}

Gfx::Path SVGLineElement::get_path(CSSPixelSize viewport_size)
{
    auto const viewport_width = viewport_size.width().to_float();
    auto const viewport_height = viewport_size.height().to_float();

    Gfx::Path path;
    float const x1 = m_x1.value_or({ 0, false }).resolve_relative_to(viewport_width);
    float const y1 = m_y1.value_or({ 0, false }).resolve_relative_to(viewport_height);
    float const x2 = m_x2.value_or({ 0, false }).resolve_relative_to(viewport_width);
    float const y2 = m_y2.value_or({ 0, false }).resolve_relative_to(viewport_height);

    // 1. perform an absolute moveto operation to absolute location (x1,y1)
    path.move_to({ x1, y1 });

    // 2. perform an absolute lineto operation to absolute location (x2,y2)
    path.line_to({ x2, y2 });

    return path;
}

// https://www.w3.org/TR/SVG11/shapes.html#LineElementX1Attribute
GC::Ref<SVGAnimatedLength> SVGLineElement::x1() const
{
    // FIXME: Populate the unit type when it is parsed (0 here is "unknown").
    // FIXME: Create a proper animated value when animations are supported.
    auto base_length = SVGLength::create(realm(), 0, m_x1.value_or({ 0, false }).value(), SVGLength::ReadOnly::No);
    auto anim_length = SVGLength::create(realm(), 0, m_x1.value_or({ 0, false }).value(), SVGLength::ReadOnly::Yes);
    return SVGAnimatedLength::create(realm(), base_length, anim_length);
}

// https://www.w3.org/TR/SVG11/shapes.html#LineElementY1Attribute
GC::Ref<SVGAnimatedLength> SVGLineElement::y1() const
{
    // FIXME: Populate the unit type when it is parsed (0 here is "unknown").
    // FIXME: Create a proper animated value when animations are supported.
    auto base_length = SVGLength::create(realm(), 0, m_y1.value_or({ 0, false }).value(), SVGLength::ReadOnly::No);
    auto anim_length = SVGLength::create(realm(), 0, m_y1.value_or({ 0, false }).value(), SVGLength::ReadOnly::Yes);
    return SVGAnimatedLength::create(realm(), base_length, anim_length);
}

// https://www.w3.org/TR/SVG11/shapes.html#LineElementX2Attribute
GC::Ref<SVGAnimatedLength> SVGLineElement::x2() const
{
    // FIXME: Populate the unit type when it is parsed (0 here is "unknown").
    // FIXME: Create a proper animated value when animations are supported.
    auto base_length = SVGLength::create(realm(), 0, m_x2.value_or({ 0, false }).value(), SVGLength::ReadOnly::No);
    auto anim_length = SVGLength::create(realm(), 0, m_x2.value_or({ 0, false }).value(), SVGLength::ReadOnly::Yes);
    return SVGAnimatedLength::create(realm(), base_length, anim_length);
}

// https://www.w3.org/TR/SVG11/shapes.html#LineElementY2Attribute
GC::Ref<SVGAnimatedLength> SVGLineElement::y2() const
{
    // FIXME: Populate the unit type when it is parsed (0 here is "unknown").
    // FIXME: Create a proper animated value when animations are supported.
    auto base_length = SVGLength::create(realm(), 0, m_y2.value_or({ 0, false }).value(), SVGLength::ReadOnly::No);
    auto anim_length = SVGLength::create(realm(), 0, m_y2.value_or({ 0, false }).value(), SVGLength::ReadOnly::Yes);
    return SVGAnimatedLength::create(realm(), base_length, anim_length);
}

}
