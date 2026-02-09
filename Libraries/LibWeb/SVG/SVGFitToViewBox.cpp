/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/SVGAnimatedRect.h>
#include <LibWeb/SVG/SVGFitToViewBox.h>

namespace Web::SVG {

void SVGFitToViewBox::initialize(JS::Realm& realm)
{
    m_view_box_for_bindings = realm.create<SVGAnimatedRect>(realm);
}

void SVGFitToViewBox::visit_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(m_view_box_for_bindings);
}

void SVGFitToViewBox::attribute_changed(DOM::Element& element, FlyString const& name, Optional<String> const& value)
{
    if (name.equals_ignoring_ascii_case(SVG::AttributeNames::viewBox)) {
        if (!value.has_value()) {
            m_view_box_for_bindings->set_nulled(true);
        } else {
            m_view_box = AttributeParser::parse_viewbox(value.value_or(String {}));
            m_view_box_for_bindings->set_nulled(!m_view_box.has_value());
            if (m_view_box.has_value()) {
                m_view_box_for_bindings->set_base_val(Gfx::DoubleRect { m_view_box->min_x, m_view_box->min_y, m_view_box->width, m_view_box->height });
                m_view_box_for_bindings->set_anim_val(Gfx::DoubleRect { m_view_box->min_x, m_view_box->min_y, m_view_box->width, m_view_box->height });
            }
        }
        if (auto layout_node = element.layout_node())
            layout_node->set_needs_layout_update(DOM::SetNeedsLayoutReason::SVGViewBoxChange);
    } else if (name.equals_ignoring_ascii_case(SVG::AttributeNames::preserveAspectRatio)) {
        m_preserve_aspect_ratio = AttributeParser::parse_preserve_aspect_ratio(value.value_or(String {}));
        if (auto layout_node = element.layout_node())
            layout_node->set_needs_layout_update(DOM::SetNeedsLayoutReason::SVGViewBoxChange);
    }
}

}
