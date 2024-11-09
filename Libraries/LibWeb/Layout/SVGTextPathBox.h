/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/SVGGraphicsBox.h>
#include <LibWeb/SVG/SVGTextPathElement.h>

namespace Web::Layout {

class SVGTextPathBox final : public SVGGraphicsBox {
    JS_CELL(SVGTextPathBox, SVGGraphicsBox);
    JS_DECLARE_ALLOCATOR(SVGTextPathBox);

public:
    SVGTextPathBox(DOM::Document&, SVG::SVGTextPathElement&, CSS::StyleProperties);
    virtual ~SVGTextPathBox() override = default;

    SVG::SVGTextPathElement& dom_node() { return static_cast<SVG::SVGTextPathElement&>(SVGGraphicsBox::dom_node()); }
    SVG::SVGTextPathElement const& dom_node() const { return static_cast<SVG::SVGTextPathElement const&>(SVGGraphicsBox::dom_node()); }

    virtual JS::GCPtr<Painting::Paintable> create_paintable() const override;

private:
    CSSPixelPoint viewbox_origin() const;
};

}
