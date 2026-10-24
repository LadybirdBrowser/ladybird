/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParsing.h>
#include <LibWeb/SVG/SVGAnimatedLengthList.h>
#include <LibWeb/SVG/SVGAnimatedNumberList.h>
#include <LibWeb/SVG/SVGLength.h>
#include <LibWeb/SVG/SVGLengthList.h>
#include <LibWeb/SVG/SVGNumber.h>
#include <LibWeb/SVG/SVGNumberList.h>
#include <LibWeb/SVG/SVGTextPositioningElement.h>

namespace Web::SVG {

SVGTextPositioningElement::SVGTextPositioningElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGTextContentElement(document, move(qualified_name))
{
}

void SVGTextPositioningElement::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_x);
    visitor.visit(m_y);
    visitor.visit(m_dx);
    visitor.visit(m_dy);
    visitor.visit(m_rotate);
}

void SVGTextPositioningElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    // https://svgwg.org/svg2-draft/types.html#SynchronizingReflectedValues
    if (name == AttributeNames::x && m_x)
        m_x->synchronize();
    else if (name == AttributeNames::y && m_y)
        m_y->synchronize();
    else if (name == AttributeNames::dx && m_dx)
        m_dx->synchronize();
    else if (name == AttributeNames::dy && m_dy)
        m_dy->synchronize();
    else if (name == AttributeNames::rotate && m_rotate)
        m_rotate->synchronize();
}

SVGTextPositioningElement::ParsedTextPositioning SVGTextPositioningElement::parsed_text_positioning() const
{
    CSS::Parser::ParsingParams const parsing_params { document() };

    // https://svgwg.org/svg2-draft/text.html#TSpanAttributes
    // FIXME: This only handles single values, not lists.
    auto parse_value = [&](Utf16FlyString const& attribute) -> Optional<SVGLengthValue> {
        auto raw_value = get_attribute_value(attribute);

        // FIXME: Should we support tree-counting and/or calculated values here?
        if (auto style_value = parse_css_type(parsing_params, raw_value, CSS::ValueType::LengthPercentage)) {
            if (auto value = SVGLengthValue::from_style_value(*style_value);
                value.has_value() && value->kind() != SVGLengthValue::Kind::Number)
                return value;
        }

        if (auto style_value = parse_css_type(parsing_params, raw_value, CSS::ValueType::Number)) {
            if (auto value = SVGLengthValue::from_style_value(*style_value);
                value.has_value() && value->kind() == SVGLengthValue::Kind::Number)
                return value;
        }

        return {};
    };

    // FIXME: Implement support for the rotate attribute.
    return {
        .x = parse_value(AttributeNames::x),
        .y = parse_value(AttributeNames::y),
        .dx = parse_value(AttributeNames::dx),
        .dy = parse_value(AttributeNames::dy),
    };
}

GC::Ref<SVGAnimatedLengthList> SVGTextPositioningElement::ensure_length_list(GC::Ptr<SVGAnimatedLengthList>& list,
    Utf16FlyString const& attribute_name, SVGLength::Directionality directionality)
{
    if (!list)
        list = SVGAnimatedLengthList::create(*this, attribute_name, directionality);
    return *list;
}

// https://svgwg.org/svg2-draft/text.html#__svg__SVGTextPositioningElement__x
GC::Ref<SVGAnimatedLengthList> SVGTextPositioningElement::x()
{
    return ensure_length_list(m_x, AttributeNames::x, SVGLength::Directionality::Horizontal);
}

// https://svgwg.org/svg2-draft/text.html#__svg__SVGTextPositioningElement__y
GC::Ref<SVGAnimatedLengthList> SVGTextPositioningElement::y()
{
    return ensure_length_list(m_y, AttributeNames::y, SVGLength::Directionality::Vertical);
}

// https://svgwg.org/svg2-draft/text.html#__svg__SVGTextPositioningElement__dx
GC::Ref<SVGAnimatedLengthList> SVGTextPositioningElement::dx()
{
    return ensure_length_list(m_dx, AttributeNames::dx, SVGLength::Directionality::Horizontal);
}

// https://svgwg.org/svg2-draft/text.html#__svg__SVGTextPositioningElement__dy
GC::Ref<SVGAnimatedLengthList> SVGTextPositioningElement::dy()
{
    return ensure_length_list(m_dy, AttributeNames::dy, SVGLength::Directionality::Vertical);
}

// https://svgwg.org/svg2-draft/text.html#__svg__SVGTextPositioningElement__rotate
GC::Ref<SVGAnimatedNumberList> SVGTextPositioningElement::rotate()
{
    if (!m_rotate)
        m_rotate = SVGAnimatedNumberList::create(*this, AttributeNames::rotate);
    return *m_rotate;
}

}
