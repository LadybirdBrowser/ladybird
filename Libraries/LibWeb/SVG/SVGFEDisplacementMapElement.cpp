/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGFEDisplacementMapElement.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/SVG/SVGAnimatedEnumeration.h>
#include <LibWeb/SVG/SVGFEDisplacementMapElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGFEDisplacementMapElement);

SVGFEDisplacementMapElement::SVGFEDisplacementMapElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, qualified_name)
{
}

void SVGFEDisplacementMapElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGFEDisplacementMapElement);
    Base::initialize(realm);
}

void SVGFEDisplacementMapElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFilterPrimitiveStandardAttributes::visit_edges(visitor);
    visitor.visit(m_in1);
    visitor.visit(m_in2);
    visitor.visit(m_scale);
    visitor.visit(m_x_channel_selector);
    visitor.visit(m_y_channel_selector);
}

// https://drafts.csswg.org/filter-effects/#dom-svgfedisplacementmapelement-in1
GC::Ref<SVGAnimatedString> SVGFEDisplacementMapElement::in1()
{
    if (!m_in1)
        m_in1 = SVGAnimatedString::create(realm(), *this, DOM::QualifiedName { AttributeNames::in, OptionalNone {}, OptionalNone {} });

    return *m_in1;
}

// https://drafts.csswg.org/filter-effects/#dom-svgfedisplacementmapelement-in2
GC::Ref<SVGAnimatedString> SVGFEDisplacementMapElement::in2()
{
    if (!m_in2)
        m_in2 = SVGAnimatedString::create(realm(), *this, DOM::QualifiedName { AttributeNames::in2, OptionalNone {}, OptionalNone {} });

    return *m_in2;
}

// https://drafts.csswg.org/filter-effects/#dom-svgfedisplacementmapelement-scale
GC::Ref<SVGAnimatedNumber> SVGFEDisplacementMapElement::scale()
{
    if (!m_scale)
        m_scale = SVGAnimatedNumber::create(realm(), *this, DOM::QualifiedName { AttributeNames::scale, OptionalNone {}, OptionalNone {} }, 0.f);

    return *m_scale;
}

// https://drafts.csswg.org/filter-effects/#element-attrdef-fedisplacementmap-xchannelselector
static SVGFEDisplacementMapElement::ChannelSelector parse_channel_selector(String const& value)
{
    if (value == "R"sv)
        return SVGFEDisplacementMapElement::ChannelSelector::Red;

    if (value == "G"sv)
        return SVGFEDisplacementMapElement::ChannelSelector::Green;

    if (value == "B"sv)
        return SVGFEDisplacementMapElement::ChannelSelector::Blue;

    if (value == "A"sv)
        return SVGFEDisplacementMapElement::ChannelSelector::Alpha;

    return SVGFEDisplacementMapElement::ChannelSelector::Alpha;
}

// https://drafts.csswg.org/filter-effects/#dom-svgfedisplacementmapelement-xchannelselector
GC::Ref<SVGAnimatedEnumeration> SVGFEDisplacementMapElement::x_channel_selector() const
{
    // FIXME: Support reflection, don't return a new object every time.
    auto x_channel_selector = parse_channel_selector(get_attribute_value(AttributeNames::xChannelSelector));
    return SVGAnimatedEnumeration::create(realm(), to_underlying(x_channel_selector));
}

// https://drafts.csswg.org/filter-effects/#dom-svgfedisplacementmapelement-ychannelselector
GC::Ref<SVGAnimatedEnumeration> SVGFEDisplacementMapElement::y_channel_selector() const
{
    // FIXME: Support reflection, don't return a new object every time.
    auto y_channel_selector = parse_channel_selector(get_attribute_value(AttributeNames::yChannelSelector));
    return SVGAnimatedEnumeration::create(realm(), to_underlying(y_channel_selector));
}

}
