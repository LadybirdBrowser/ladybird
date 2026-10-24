/*
 * Copyright (c) 2026, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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

void SVGFEDisplacementMapElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    SVGFilterPrimitiveStandardAttributes::visit_edges(visitor);
    visitor.visit(m_in1);
    visitor.visit(m_in2);
    visitor.visit(m_scale);
}

// https://drafts.csswg.org/filter-effects/#dom-svgfedisplacementmapelement-in1
GC::Ref<SVGAnimatedString> SVGFEDisplacementMapElement::in1()
{
    if (!m_in1)
        m_in1 = SVGAnimatedString::create(*this, DOM::QualifiedName { AttributeNames::in, OptionalNone {}, OptionalNone {} });

    return *m_in1;
}

// https://drafts.csswg.org/filter-effects/#dom-svgfedisplacementmapelement-in2
GC::Ref<SVGAnimatedString> SVGFEDisplacementMapElement::in2()
{
    if (!m_in2)
        m_in2 = SVGAnimatedString::create(*this, DOM::QualifiedName { AttributeNames::in2, OptionalNone {}, OptionalNone {} });

    return *m_in2;
}

// https://drafts.csswg.org/filter-effects/#dom-svgfedisplacementmapelement-scale
GC::Ref<SVGAnimatedNumber> SVGFEDisplacementMapElement::scale()
{
    if (!m_scale)
        m_scale = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::scale, OptionalNone {}, OptionalNone {} }, 0.f);

    return *m_scale;
}

// https://drafts.csswg.org/filter-effects/#dom-svgfedisplacementmapelement-xchannelselector
static constexpr Array channel_selector_keywords {
    SVGAnimatedEnumeration::Keyword { "R"sv, to_underlying(SVGFEDisplacementMapElement::ChannelSelector::Red) },
    SVGAnimatedEnumeration::Keyword { "G"sv, to_underlying(SVGFEDisplacementMapElement::ChannelSelector::Green) },
    SVGAnimatedEnumeration::Keyword { "B"sv, to_underlying(SVGFEDisplacementMapElement::ChannelSelector::Blue) },
    SVGAnimatedEnumeration::Keyword { "A"sv, to_underlying(SVGFEDisplacementMapElement::ChannelSelector::Alpha) },
};

GC::Ref<SVGAnimatedEnumeration> SVGFEDisplacementMapElement::x_channel_selector()
{
    return svg_animated_enumeration_for_attribute(AttributeNames::xChannelSelector, channel_selector_keywords, to_underlying(ChannelSelector::Alpha));
}

// https://drafts.csswg.org/filter-effects/#dom-svgfedisplacementmapelement-ychannelselector
GC::Ref<SVGAnimatedEnumeration> SVGFEDisplacementMapElement::y_channel_selector()
{
    return svg_animated_enumeration_for_attribute(AttributeNames::yChannelSelector, channel_selector_keywords, to_underlying(ChannelSelector::Alpha));
}

}
