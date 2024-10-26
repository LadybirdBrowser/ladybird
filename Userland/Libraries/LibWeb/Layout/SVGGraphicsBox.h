/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/SVGBox.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>

namespace Web::Layout {

class SVGGraphicsBox : public SVGBox {
    JS_CELL(SVGGraphicsBox, SVGBox);

public:
    SVGGraphicsBox(DOM::Document&, SVG::SVGGraphicsElement&, CSS::StyleProperties);
    virtual ~SVGGraphicsBox() override = default;

    SVG::SVGGraphicsElement& dom_node() { return verify_cast<SVG::SVGGraphicsElement>(SVGBox::dom_node()); }
    SVG::SVGGraphicsElement const& dom_node() const { return verify_cast<SVG::SVGGraphicsElement>(SVGBox::dom_node()); }

    virtual JS::GCPtr<Painting::Paintable> create_paintable() const override;
};

}
