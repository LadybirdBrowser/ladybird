/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGStopElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGStopElement);

SVGStopElement::SVGStopElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, qualified_name)
{
}

Gfx::Color SVGStopElement::stop_color()
{
    if (auto const* values = style_group<CSS::ComputedValues::SVGResetValues>())
        return Gfx::Color::from_bgra(values->stop_color);
    return CSS::InitialValues::stop_color();
}

float SVGStopElement::stop_opacity() const
{
    if (auto const* values = style_group<CSS::ComputedValues::SVGResetValues>())
        return values->stop_opacity;
    return 1;
}

// https://svgwg.org/svg2-draft/pservers.html#StopElementOffsetAttribute
GC::Ref<SVGAnimatedNumber> SVGStopElement::offset()
{
    if (!m_stop_offset)
        m_stop_offset = SVGAnimatedNumber::create(*this, DOM::QualifiedName { AttributeNames::offset, OptionalNone {}, OptionalNone {} }, 0.f);
    return *m_stop_offset;
}

void SVGStopElement::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_stop_offset);
}

}
