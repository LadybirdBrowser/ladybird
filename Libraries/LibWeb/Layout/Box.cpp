/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/FormattingContext.h>
#include <LibWeb/Layout/TableWrapper.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Layout {

Box::Box(DOM::Document& document, DOM::Node* node, GC::Ref<CSS::ComputedProperties> style)
    : NodeWithStyleAndBoxModelMetrics(document, node, move(style))
{
}

Box::Box(DOM::Document& document, DOM::Node* node, NonnullOwnPtr<CSS::ComputedValues> computed_values)
    : NodeWithStyleAndBoxModelMetrics(document, node, move(computed_values))
{
}

Box::~Box()
{
}

CSS::SizeWithAspectRatio Box::intrinsic_content_box_size() const
{
    // https://www.w3.org/TR/css-contain-2/#containment-size
    // Replaced elements must be treated as having a natural width and height of 0 and no natural aspect
    // ratio.
    if (has_size_containment())
        return { 0, 0, {} };

    // Return the content-box intrinsic size
    return compute_intrinsic_content_box_size();
}

void Box::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_contained_abspos_children);
}

GC::Ptr<Painting::Paintable> Box::create_paintable() const
{
    return Painting::PaintableBox::create(*this);
}

Painting::PaintableBox* Box::paintable_box()
{
    return static_cast<Painting::PaintableBox*>(Node::first_paintable());
}

Painting::PaintableBox const* Box::paintable_box() const
{
    return static_cast<Painting::PaintableBox const*>(Node::first_paintable());
}

Optional<CSSPixelFraction> Box::preferred_aspect_ratio() const
{
    auto const& aspect = computed_values().aspect_ratio();

    // https://www.w3.org/TR/css-contain-2/#containment-size

    if (!has_size_containment() && aspect.use_natural_aspect_ratio_if_available) {
        if (auto intrinsic = intrinsic_content_box_size(); intrinsic.has_aspect_ratio())
            return intrinsic.aspect_ratio;
    }

    if (aspect.preferred_ratio.has_value()) {
        auto ratio = aspect.preferred_ratio.value();
        if (!ratio.is_degenerate())
            return CSSPixelFraction(ratio.numerator(), ratio.denominator());
    }

    return {};
}

}
