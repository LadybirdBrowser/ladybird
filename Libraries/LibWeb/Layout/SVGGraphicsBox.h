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
    GC_CELL(SVGGraphicsBox, SVGBox);

public:
    SVGGraphicsBox(DOM::Document&, SVG::SVGGraphicsElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~SVGGraphicsBox() override = default;

    SVG::SVGGraphicsElement& dom_node() { return as<SVG::SVGGraphicsElement>(SVGBox::dom_node()); }
    SVG::SVGGraphicsElement const& dom_node() const { return as<SVG::SVGGraphicsElement>(SVGBox::dom_node()); }

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;
};

}
