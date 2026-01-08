/*
 * Copyright (c) 2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGFEFloodElementPrototype.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/SVGGraphicsBox.h>
#include <LibWeb/SVG/SVGFEFloodElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEFloodElement);

SVGFEFloodElement::SVGFEFloodElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, qualified_name)
{
}

void SVGFEFloodElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGFEFloodElement);
    Base::initialize(realm);
}

void SVGFEFloodElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFilterPrimitiveStandardAttributes::visit_edges(visitor);
}

GC::Ptr<Layout::Node> SVGFEFloodElement::create_layout_node(GC::Ref<CSS::ComputedProperties> style)
{
    return heap().allocate<Layout::SVGBox>(document(), *this, move(style));
}

// https://www.w3.org/TR/filter-effects-1/#FloodColorProperty
Gfx::Color SVGFEFloodElement::flood_color()
{
    VERIFY(computed_properties());
    return computed_properties()->color_or_fallback(CSS::PropertyID::FloodColor, CSS::ColorResolutionContext::for_element({ *this }), CSS::InitialValues::flood_color());
}

// https://www.w3.org/TR/filter-effects-1/#FloodOpacityProperty
float SVGFEFloodElement::flood_opacity() const
{
    VERIFY(computed_properties());
    return computed_properties()->flood_opacity();
}

}
