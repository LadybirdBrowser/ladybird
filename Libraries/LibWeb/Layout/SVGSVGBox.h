/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Rect.h>
#include <LibWeb/Layout/ReplacedBox.h>
#include <LibWeb/SVG/SVGSVGElement.h>

namespace Web::Layout {

class SVGSVGBox final : public ReplacedBox {
    LAYOUT_NODE(SVGSVGBox, ReplacedBox);

public:
    SVGSVGBox(DOM::Document&, SVG::SVGSVGElement&, CSS::LayoutStyle);
    virtual ~SVGSVGBox() override = default;

    SVG::SVGSVGElement& dom_node() { return as<SVG::SVGSVGElement>(*ReplacedBox::dom_node()); }
    SVG::SVGSVGElement const& dom_node() const { return as<SVG::SVGSVGElement>(*ReplacedBox::dom_node()); }

    Gfx::FloatRect view_box_or_viewport_rect() const;

private:
    virtual CSS::SizeWithAspectRatio natural_size() const override;
};

template<>
inline bool Node::fast_is<SVGSVGBox>() const { return is_svg_svg_box(); }

}
