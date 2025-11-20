/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGTextPositioningElementPrototype.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/PercentageStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParser.h>
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

void SVGTextPositioningElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGTextPositioningElement);
    Base::initialize(realm);
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

void SVGTextPositioningElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    if (name == AttributeNames::x)
        m_x = {};
    else if (name == AttributeNames::y)
        m_y = {};
    else if (name == AttributeNames::dx)
        m_dx = {};
    else if (name == AttributeNames::dy)
        m_dy = {};
    else if (name == AttributeNames::rotate)
        m_rotate = {};
}

TextPositioning SVGTextPositioningElement::text_positioning() const
{
    CSS::Parser::ParsingParams const parsing_params { document() };

    // https://svgwg.org/svg2-draft/text.html#TSpanAttributes
    // FIXME: This only handles single values, not lists.
    auto resolve_value = [&](FlyString const& attribute) -> Vector<TextPositioning::Position> {
        auto raw_value = get_attribute_value(attribute);

        auto style_value = parse_css_type(parsing_params, raw_value, CSS::ValueType::LengthPercentage);
        if (auto const* length_style_value = as_if<CSS::LengthStyleValue>(style_value.ptr()))
            return { CSS::LengthPercentage::from_style_value(*length_style_value) };

        if (auto const* percentage_style_value = as_if<CSS::PercentageStyleValue>(style_value.ptr()))
            return { CSS::LengthPercentage::from_style_value(*percentage_style_value) };

        style_value = parse_css_type(parsing_params, raw_value, CSS::ValueType::Number);
        if (auto const* number_style_value = as_if<CSS::NumberStyleValue>(style_value.ptr()))
            return { CSS::Number { CSS::Number::Type::Number, number_style_value->number() } };

        return {};
    };

    // FIXME: Implement support for the rotate attribute.
    return {
        .x = resolve_value(AttributeNames::x),
        .y = resolve_value(AttributeNames::y),
        .dx = resolve_value(AttributeNames::dx),
        .dy = resolve_value(AttributeNames::dy),
        .rotate = Vector<float> {},
    };
}

GC::Ref<SVGAnimatedLengthList> SVGTextPositioningElement::ensure_length_list(GC::Ptr<SVGAnimatedLengthList>& list,
    FlyString const& attribute_name) const
{
    if (!list) {
        // FIXME: This only handles single values, not lists.
        float value = 0.f;
        auto maybe_number_percentage = AttributeParser::parse_number_percentage(get_attribute_value(attribute_name));
        if (maybe_number_percentage.has_value())
            value = maybe_number_percentage.release_value().value();

        auto length = SVGLength::create(realm(), SVGLength::SVG_LENGTHTYPE_NUMBER, value, SVGLength::ReadOnly::Yes);
        auto length_list = SVGLengthList::create(realm(), { length }, ReadOnlyList::Yes);
        list = SVGAnimatedLengthList::create(realm(), length_list);
    }
    return *list;
}

// https://svgwg.org/svg2-draft/text.html#__svg__SVGTextPositioningElement__x
GC::Ref<SVGAnimatedLengthList> SVGTextPositioningElement::x()
{
    return ensure_length_list(m_x, AttributeNames::x);
}

// https://svgwg.org/svg2-draft/text.html#__svg__SVGTextPositioningElement__y
GC::Ref<SVGAnimatedLengthList> SVGTextPositioningElement::y()
{
    return ensure_length_list(m_y, AttributeNames::y);
}

// https://svgwg.org/svg2-draft/text.html#__svg__SVGTextPositioningElement__dx
GC::Ref<SVGAnimatedLengthList> SVGTextPositioningElement::dx()
{
    return ensure_length_list(m_dx, AttributeNames::dx);
}

// https://svgwg.org/svg2-draft/text.html#__svg__SVGTextPositioningElement__dy
GC::Ref<SVGAnimatedLengthList> SVGTextPositioningElement::dy()
{
    return ensure_length_list(m_dy, AttributeNames::dy);
}

// https://svgwg.org/svg2-draft/text.html#__svg__SVGTextPositioningElement__rotate
GC::Ref<SVGAnimatedNumberList> SVGTextPositioningElement::rotate()
{
    if (!m_rotate) {
        // FIXME: This only handles single values, not lists.
        float value = 0.f;
        auto maybe_number_percentage = AttributeParser::parse_number_percentage(get_attribute_value(AttributeNames::rotate));
        if (maybe_number_percentage.has_value() && !maybe_number_percentage.value().is_percentage())
            value = maybe_number_percentage.release_value().value();

        auto number = SVGNumber::create(realm(), value, SVGNumber::ReadOnly::Yes);
        auto number_list = SVGNumberList::create(realm(), { number }, ReadOnlyList::Yes);
        m_rotate = SVGAnimatedNumberList::create(realm(), number_list);
    }
    return *m_rotate;
}

}
