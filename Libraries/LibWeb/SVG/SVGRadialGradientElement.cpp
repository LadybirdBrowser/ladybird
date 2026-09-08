/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGRadialGradientElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGRadialGradientElement);

SVGRadialGradientElement::SVGRadialGradientElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGradientElement(document, qualified_name)
{
}

void SVGRadialGradientElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    // FIXME: These are <length> or <coordinate> in the spec, but all examples seem to allow percentages
    // and unitless values.
    if (name == SVG::AttributeNames::cx) {
        m_cx = parse_number_percentage(value.value_or({}));
    } else if (name == SVG::AttributeNames::cy) {
        m_cy = parse_number_percentage(value.value_or({}));
    } else if (name == SVG::AttributeNames::fx) {
        m_fx = parse_number_percentage(value.value_or({}));
    } else if (name == SVG::AttributeNames::fy) {
        m_fy = parse_number_percentage(value.value_or({}));
    } else if (name == SVG::AttributeNames::fr) {
        m_fr = parse_number_percentage(value.value_or({}));
    } else if (name == SVG::AttributeNames::r) {
        m_r = parse_number_percentage(value.value_or({}));
    }
}

void SVGRadialGradientElement::collect_gradient_attributes(GradientAttributes& attributes) const
{
    if (!attributes.cx.has_value() && m_cx.has_value())
        attributes.cx = m_cx;
    if (!attributes.cy.has_value() && m_cy.has_value())
        attributes.cy = m_cy;
    if (!attributes.r.has_value() && m_r.has_value() && m_r->value() >= 0)
        attributes.r = m_r;
    if (!attributes.fx.has_value() && m_fx.has_value())
        attributes.fx = m_fx;
    if (!attributes.fy.has_value() && m_fy.has_value())
        attributes.fy = m_fy;
    if (!attributes.fr.has_value() && m_fr.has_value() && m_fr->value() >= 0)
        attributes.fr = m_fr;
}

}
