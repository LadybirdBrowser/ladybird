/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/SVGBox.h>
#include <LibWeb/SVG/SVGClipPathElement.h>
#include <LibWeb/SVG/SVGElement.h>

namespace Web::Layout {

class SVGClipBox : public SVGBox {
    JS_CELL(SVGClipBox, SVGBox);
    JS_DECLARE_ALLOCATOR(SVGClipBox);

public:
    SVGClipBox(DOM::Document&, SVG::SVGClipPathElement&, CSS::StyleProperties);
    virtual ~SVGClipBox() override = default;

    SVG::SVGClipPathElement& dom_node() { return verify_cast<SVG::SVGClipPathElement>(SVGBox::dom_node()); }
    SVG::SVGClipPathElement const& dom_node() const { return verify_cast<SVG::SVGClipPathElement>(SVGBox::dom_node()); }

    virtual JS::GCPtr<Painting::Paintable> create_paintable() const override;
};

}
