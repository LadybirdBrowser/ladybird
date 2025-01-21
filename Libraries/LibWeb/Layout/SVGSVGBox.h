/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/ReplacedBox.h>
#include <LibWeb/SVG/SVGSVGElement.h>

namespace Web::Layout {

class SVGSVGBox final : public ReplacedBox {
    GC_CELL(SVGSVGBox, ReplacedBox);
    GC_DECLARE_ALLOCATOR(SVGSVGBox);

public:
    SVGSVGBox(DOM::Document&, SVG::SVGSVGElement&, GC::Ref<CSS::ComputedProperties>);
    virtual ~SVGSVGBox() override = default;

    SVG::SVGSVGElement& dom_node() { return as<SVG::SVGSVGElement>(ReplacedBox::dom_node()); }
    SVG::SVGSVGElement const& dom_node() const { return as<SVG::SVGSVGElement>(ReplacedBox::dom_node()); }

    virtual bool can_have_children() const override { return true; }

    virtual GC::Ptr<Painting::Paintable> create_paintable() const override;

    virtual void prepare_for_replaced_layout() override;

private:
    virtual bool is_svg_svg_box() const final { return true; }

    [[nodiscard]] Optional<CSSPixelFraction> calculate_intrinsic_aspect_ratio() const;
};

template<>
inline bool Node::fast_is<SVGSVGBox>() const { return is_svg_svg_box(); }

}
