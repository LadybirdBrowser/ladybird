/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/SVGStopElement.h>
#include <LibWeb/CSS/ComputedProperties.h>
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
    if (auto computed_values = this->computed_values())
        return computed_values->stop_color();
    return CSS::InitialValues::stop_color();
}

float SVGStopElement::stop_opacity() const
{
    if (auto computed_values = this->computed_values())
        return computed_values->stop_opacity();
    return 1;
}

// https://svgwg.org/svg2-draft/pservers.html#StopElementOffsetAttribute
GC::Ref<SVGAnimatedNumber> SVGStopElement::offset()
{
    if (!m_stop_offset)
        m_stop_offset = SVGAnimatedNumber::create(realm(), *this, DOM::QualifiedName { AttributeNames::offset, OptionalNone {}, OptionalNone {} }, 0.f);
    return *m_stop_offset;
}

void SVGStopElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGStopElement);
    Base::initialize(realm);
}

void SVGStopElement::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_stop_offset);
}

}
