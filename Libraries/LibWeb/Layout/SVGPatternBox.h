/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/SVGBox.h>
#include <LibWeb/SVG/SVGPatternElement.h>

namespace Web::Layout {

class SVGPatternBox final : public SVGBox {
    LAYOUT_NODE(SVGPatternBox, SVGBox);

public:
    SVGPatternBox(DOM::Document&, SVG::SVGPatternElement&, CSS::ComputedProperties const&);
    virtual ~SVGPatternBox() override = default;

    SVG::SVGPatternElement& dom_node() { return as<SVG::SVGPatternElement>(SVGBox::dom_node()); }
    SVG::SVGPatternElement const& dom_node() const { return as<SVG::SVGPatternElement>(SVGBox::dom_node()); }

    virtual RefPtr<Painting::Paintable> create_paintable() const override;

private:
    virtual bool is_svg_pattern_box() const final { return true; }
};

template<>
inline bool Node::fast_is<SVGPatternBox>() const { return is_svg_pattern_box(); }

}
