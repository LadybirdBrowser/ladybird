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

Optional<CSSPixels> Box::natural_width() const
{
    // https://drafts.csswg.org/css-contain-2/#containment-size
    // Replaced elements must be treated as having a natural width and height of 0 and no natural aspect
    // ratio.
    if (m_has_size_containment)
        return 0;
    return m_natural_width;
}
Optional<CSSPixels> Box::natural_height() const
{
    // https://drafts.csswg.org/css-contain-2/#containment-size
    // Replaced elements must be treated as having a natural width and height of 0 and no natural aspect
    // ratio.
    if (m_has_size_containment)
        return 0;
    return m_natural_height;
}
Optional<CSSPixelFraction> Box::natural_aspect_ratio() const
{
    // https://drafts.csswg.org/css-contain-2/#containment-size
    // Replaced elements must be treated as having a natural width and height of 0 and no natural aspect
    // ratio.
    if (m_has_size_containment)
        return {};
    return m_natural_aspect_ratio;
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
    auto computed_aspect_ratio = computed_values().aspect_ratio();
    if (computed_aspect_ratio.use_natural_aspect_ratio_if_available && natural_aspect_ratio().has_value())
        return natural_aspect_ratio();

    if (!computed_aspect_ratio.preferred_ratio.has_value())
        return {};

    auto ratio = computed_aspect_ratio.preferred_ratio.release_value();
    if (ratio.is_degenerate())
        return {};

    return CSSPixelFraction(ratio.numerator(), ratio.denominator());
}

}
