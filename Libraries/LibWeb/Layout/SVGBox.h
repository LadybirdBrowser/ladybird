/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>

namespace Web::Layout {

class SVGBox : public Box {
    GC_CELL(SVGBox, Box);

public:
    SVGBox(DOM::Document&, SVG::SVGElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~SVGBox() override = default;

    SVG::SVGElement& dom_node() { return as<SVG::SVGElement>(*Box::dom_node()); }
    SVG::SVGElement const& dom_node() const { return as<SVG::SVGElement>(*Box::dom_node()); }

private:
    virtual bool is_svg_box() const final { return true; }
};

template<>
inline bool Node::fast_is<SVGBox>() const { return is_svg_box(); }

}
