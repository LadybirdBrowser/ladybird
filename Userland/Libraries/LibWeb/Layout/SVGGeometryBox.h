/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/Layout/SVGGraphicsBox.h>
#include <LibWeb/SVG/SVGGeometryElement.h>

namespace Web::Layout {

class SVGGeometryBox final : public SVGGraphicsBox {
    JS_CELL(SVGGeometryBox, SVGGraphicsBox);
    JS_DECLARE_ALLOCATOR(SVGGeometryBox);

public:
    SVGGeometryBox(DOM::Document&, SVG::SVGGeometryElement&, CSS::StyleProperties);
    virtual ~SVGGeometryBox() override = default;

    SVG::SVGGeometryElement& dom_node() { return static_cast<SVG::SVGGeometryElement&>(SVGGraphicsBox::dom_node()); }
    SVG::SVGGeometryElement const& dom_node() const { return static_cast<SVG::SVGGeometryElement const&>(SVGGraphicsBox::dom_node()); }

    virtual JS::GCPtr<Painting::Paintable> create_paintable() const override;

private:
    virtual bool is_svg_geometry_box() const final { return true; }
};

template<>
inline bool Node::fast_is<SVGGeometryBox>() const { return is_svg_geometry_box(); }

}
