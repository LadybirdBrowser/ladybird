/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGAnimatedRect.h>
#include <LibWeb/SVG/SVGFitToViewBox.h>

namespace Web::SVG {

void SVGFitToViewBox::initialize_fit_to_view_box(SVGElement& element)
{
    m_view_box_for_bindings = SVGAnimatedRect::create();
    m_preserve_aspect_ratio_for_bindings = SVGAnimatedPreserveAspectRatio::create(element);
}

void SVGFitToViewBox::visit_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(m_view_box_for_bindings);
    visitor.visit(m_preserve_aspect_ratio_for_bindings);
}

void SVGFitToViewBox::attribute_changed(DOM::Element& element, Utf16FlyString const& name, Optional<Utf16String> const& value)
{
    if (name.equals_ignoring_ascii_case(SVG::AttributeNames::viewBox)) {
        // https://svgwg.org/svg2-draft/types.html#InterfaceSVGAnimatedRect
        // 1. Let value be the value of the reflected attribute (using the attribute's initial value if it is not present or invalid).
        m_view_box = parse_viewbox(value.value_or({}));

        // 2. Let x, y, width and height be those corresponding components of value.
        auto rect = m_view_box.has_value()
            ? Gfx::DoubleRect { m_view_box->min_x, m_view_box->min_y, m_view_box->width, m_view_box->height }
            : Gfx::DoubleRect {};

        // 3. Set the DOMRect object's x coordinate, y coordinate, width and height to x, y, width and height, respectively.
        m_view_box_for_bindings->set_base_val(rect);
        m_view_box_for_bindings->set_anim_val(rect);
        element.set_needs_layout_update(DOM::SetNeedsLayoutReason::SVGViewBoxChange);
    } else if (name.equals_ignoring_ascii_case(SVG::AttributeNames::preserveAspectRatio)) {
        m_preserve_aspect_ratio = parse_preserve_aspect_ratio(value.value_or({}));
        element.set_needs_layout_update(DOM::SetNeedsLayoutReason::SVGViewBoxChange);
    }
}

}
