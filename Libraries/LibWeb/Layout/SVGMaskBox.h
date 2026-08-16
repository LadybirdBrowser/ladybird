/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/SVGGraphicsBox.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGMaskElement.h>

namespace Web::Layout {

class SVGMaskBox : public SVGGraphicsBox {
    LAYOUT_NODE(SVGMaskBox, SVGGraphicsBox);

public:
    SVGMaskBox(DOM::Document&, SVG::SVGMaskElement&, CSS::LayoutStyle);
    virtual ~SVGMaskBox() override = default;

    SVG::SVGMaskElement& dom_node() { return as<SVG::SVGMaskElement>(SVGGraphicsBox::dom_node()); }
    SVG::SVGMaskElement const& dom_node() const { return as<SVG::SVGMaskElement>(SVGGraphicsBox::dom_node()); }
};

template<>
inline bool Node::fast_is<SVGMaskBox>() const { return is_svg_mask_box(); }

}
