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
    GC_CELL(SVGMaskBox, SVGGraphicsBox);
    GC_DECLARE_ALLOCATOR(SVGMaskBox);

public:
    SVGMaskBox(DOM::Document&, SVG::SVGMaskElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~SVGMaskBox() override = default;

    virtual bool is_svg_mask_box() const override { return true; }

    SVG::SVGMaskElement& dom_node() { return as<SVG::SVGMaskElement>(SVGGraphicsBox::dom_node()); }
    SVG::SVGMaskElement const& dom_node() const { return as<SVG::SVGMaskElement>(SVGGraphicsBox::dom_node()); }

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;
};

template<>
inline bool Node::fast_is<SVGMaskBox>() const { return is_svg_mask_box(); }

}
