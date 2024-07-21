/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16View.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Utf16String.h>
#include <LibWeb/Bindings/SVGTextPositioningElementPrototype.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGGeometryElement.h>
#include <LibWeb/SVG/SVGTextPositioningElement.h>

namespace Web::SVG {

SVGTextPositioningElement::SVGTextPositioningElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGTextContentElement(document, move(qualified_name))
{
}

void SVGTextPositioningElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGTextPositioningElement);
}

void SVGTextPositioningElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value)
{
    SVGGraphicsElement::attribute_changed(name, old_value, value);

    if (name == SVG::AttributeNames::x) {
        m_x = AttributeParser::parse_number_percentage(value.value_or(String {}));
    } else if (name == SVG::AttributeNames::y) {
        m_y = AttributeParser::parse_number_percentage(value.value_or(String {}));
    } else if (name == SVG::AttributeNames::dx) {
        m_dx = AttributeParser::parse_number_percentage(value.value_or(String {}));
    } else if (name == SVG::AttributeNames::dy) {
        m_dy = AttributeParser::parse_number_percentage(value.value_or(String {}));
    }
}

Gfx::FloatPoint SVGTextPositioningElement::get_offset(CSSPixelSize const& viewport_size) const
{
    auto const viewport_width = viewport_size.width().to_float();
    auto const viewport_height = viewport_size.height().to_float();

    float const x = m_x.value_or({ 0, false }).resolve_relative_to(viewport_width);
    float const y = m_y.value_or({ 0, false }).resolve_relative_to(viewport_height);
    float const dx = m_dx.value_or({ 0, false }).resolve_relative_to(viewport_width);
    float const dy = m_dy.value_or({ 0, false }).resolve_relative_to(viewport_height);

    return { x + dx, y + dy };
}

}
