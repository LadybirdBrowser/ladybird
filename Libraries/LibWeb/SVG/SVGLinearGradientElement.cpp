/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParsing.h>
#include <LibWeb/SVG/SVGLinearGradientElement.h>
#include <LibWeb/SVG/SVGStopElement.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGLinearGradientElement);

SVGLinearGradientElement::SVGLinearGradientElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGGradientElement(document, qualified_name)
{
}

void SVGLinearGradientElement::attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_)
{
    Base::attribute_changed(name, old_value, value, namespace_);

    // FIXME: Should allow for `<number-percentage> | <length>` for x1, x2, y1, y2
    if (name == SVG::AttributeNames::x1) {
        m_x1 = parse_number_percentage(value.value_or({}));
    } else if (name == SVG::AttributeNames::y1) {
        m_y1 = parse_number_percentage(value.value_or({}));
    } else if (name == SVG::AttributeNames::x2) {
        m_x2 = parse_number_percentage(value.value_or({}));
    } else if (name == SVG::AttributeNames::y2) {
        m_y2 = parse_number_percentage(value.value_or({}));
    }
}

void SVGLinearGradientElement::collect_gradient_attributes(GradientAttributes& attributes) const
{
    if (!attributes.x1.has_value() && m_x1.has_value())
        attributes.x1 = m_x1;
    if (!attributes.y1.has_value() && m_y1.has_value())
        attributes.y1 = m_y1;
    if (!attributes.x2.has_value() && m_x2.has_value())
        attributes.x2 = m_x2;
    if (!attributes.y2.has_value() && m_y2.has_value())
        attributes.y2 = m_y2;
}

}
