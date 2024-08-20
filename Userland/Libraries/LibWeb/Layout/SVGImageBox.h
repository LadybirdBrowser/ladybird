/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/Layout/SVGGraphicsBox.h>
#include <LibWeb/SVG/SVGImageElement.h>

namespace Web::Layout {

class SVGImageBox : public SVGGraphicsBox {
    JS_CELL(SVGImageBox, SVGGraphicsBox);

public:
    SVGImageBox(DOM::Document&, SVG::SVGGraphicsElement&, NonnullRefPtr<CSS::StyleProperties>);
    virtual ~SVGImageBox() override = default;

    SVG::SVGImageElement& dom_node() { return static_cast<SVG::SVGImageElement&>(SVGGraphicsBox::dom_node()); }
    SVG::SVGImageElement const& dom_node() const { return static_cast<SVG::SVGImageElement const&>(SVGGraphicsBox::dom_node()); }

    virtual JS::GCPtr<Painting::Paintable> create_paintable() const override;
};

}
