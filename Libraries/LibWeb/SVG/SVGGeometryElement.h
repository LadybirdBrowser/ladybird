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
    virtual bool is_svg_geometry_element() const final { return true; }

    virtual CSS::ElementBoxKind box_kind() const override;

    Gfx::Path computed_path();

    WebIDL::ExceptionOr<bool> is_point_in_fill(Bindings::DOMPointInit const&);
    WebIDL::ExceptionOr<bool> is_point_in_stroke(Bindings::DOMPointInit const&);
    WebIDL::ExceptionOr<float> get_total_length();
    WebIDL::ExceptionOr<GC::Ref<Geometry::DOMPoint>> get_point_at_length(float distance);

    GC::Ref<SVGAnimatedNumber> path_length();

protected:
    SVGGeometryElement(DOM::Document& document, DOM::QualifiedName qualified_name);
    virtual void visit_edges(Cell::Visitor&) override;

private:
    bool with_computed_values(Function<void(CSSPixelSize viewport_size, CSS::ComputedValues const&)> const&);

    GC::Ptr<SVGAnimatedNumber> m_path_length;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<SVG::SVGGeometryElement>() const { return is_svg_geometry_element(); }

}
