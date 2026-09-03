/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Geometry/DOMPoint.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>

namespace Web::SVG {

// https://svgwg.org/svg2-draft/types.html#InterfaceSVGGeometryElement
class SVGGeometryElement : public SVGGraphicsElement {
    WEB_WRAPPABLE(SVGGeometryElement, SVGGraphicsElement);

public:
    virtual Layout::Node* create_layout_node(CSS::LayoutStyle) override;

    virtual Gfx::Path get_path(CSSPixelSize viewport_size, CSS::ComputedValues const&) = 0;

    WebIDL::ExceptionOr<float> get_total_length();
    GC::Ref<Geometry::DOMPoint> get_point_at_length(float distance);

    GC::Ref<SVGAnimatedNumber> path_length();

protected:
    SVGGeometryElement(DOM::Document& document, DOM::QualifiedName qualified_name);
    virtual void visit_edges(Cell::Visitor&) override;

private:
    GC::Ptr<SVGAnimatedNumber> m_path_length;
};

}
