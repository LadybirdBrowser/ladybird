/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/Layout/SVGGraphicsBox.h>
#include <LibWeb/SVG/SVGTextPositioningElement.h>

namespace Web::Layout {

class SVGTextBox final : public SVGGraphicsBox {
    JS_CELL(SVGTextBox, SVGGraphicsBox);
    JS_DECLARE_ALLOCATOR(SVGTextBox);

public:
    SVGTextBox(DOM::Document&, SVG::SVGTextPositioningElement&, CSS::StyleProperties);
    virtual ~SVGTextBox() override = default;

    SVG::SVGTextPositioningElement& dom_node() { return static_cast<SVG::SVGTextPositioningElement&>(SVGGraphicsBox::dom_node()); }
    SVG::SVGTextPositioningElement const& dom_node() const { return static_cast<SVG::SVGTextPositioningElement const&>(SVGGraphicsBox::dom_node()); }

    virtual JS::GCPtr<Painting::Paintable> create_paintable() const override;

private:
    CSSPixelPoint viewbox_origin() const;
};

}
