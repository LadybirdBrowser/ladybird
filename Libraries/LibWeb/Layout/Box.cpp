/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/Layout/AbsposLayoutInputs.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/FormattingContext.h>
#include <LibWeb/Layout/TableWrapper.h>
#include <LibWeb/Painting/Paintable.h>

namespace Web::Layout {

Box::Box(DOM::Document& document, DOM::Node* node, CSS::ComputedProperties const& style)
    : NodeWithStyleAndBoxModelMetrics(document, node, style)
{
}

Box::Box(DOM::Document& document, DOM::Node* node, NonnullOwnPtr<CSS::ComputedValues> computed_values)
    : NodeWithStyleAndBoxModelMetrics(document, node, move(computed_values))
{
}

Box::~Box()
{
}

void Box::set_saved_abspos_layout_inputs(AbsposLayoutInputs const& abspos_layout_inputs)
{
    if (m_saved_abspos_layout_inputs)
        *m_saved_abspos_layout_inputs = abspos_layout_inputs;
    else
        m_saved_abspos_layout_inputs = make<AbsposLayoutInputs>(abspos_layout_inputs);
}

void Box::clear_saved_abspos_layout_inputs()
{
    m_saved_abspos_layout_inputs = nullptr;
}

bool Box::is_partial_relayout_boundary() const
{
    // An SVG root's used size is determined solely by its own attributes and outer context,
    // never by its children, so its size and position from the previous layout can be reused.
    // A nested <svg> does not qualify: its subtree is laid out in the outer SVG's
    // viewBox-transformed coordinate system, which a relayout rooted at the inner <svg>
    // cannot reproduce.
    if (is_svg_svg_box())
        return !(parent() && (parent()->is_svg_box() || parent()->is_svg_svg_box()));

    return false;
}

CSS::SizeWithAspectRatio Box::auto_content_box_size() const
{
    // https://drafts.csswg.org/css-contain-2/#containment-size
    // Replaced elements must be treated as having a natural width and height of 0 and no natural aspect
    // ratio.
    if (has_size_containment())
        return { 0, 0, {} };

    return compute_auto_content_box_size();
}

RefPtr<Painting::Paintable> Box::create_paintable() const
{
    return Painting::Paintable::create(*this);
}

RefPtr<Painting::Paintable> Box::paintable_box()
{
    if (auto paintable = Node::paintable())
        return static_cast<Painting::Paintable&>(*paintable);
    return nullptr;
}

RefPtr<Painting::Paintable const> Box::paintable_box() const
{
    if (auto paintable = Node::paintable())
        return static_cast<Painting::Paintable const&>(*paintable);
    return nullptr;
}

Optional<CSSPixelFraction> Box::preferred_aspect_ratio() const
{
    auto const& computed_aspect_ratio = computed_values().aspect_ratio();

    // https://www.w3.org/TR/css-contain-2/#containment-size
    if (!has_size_containment() && computed_aspect_ratio.use_natural_aspect_ratio_if_available) {
        if (auto auto_size = auto_content_box_size(); auto_size.has_aspect_ratio())
            return auto_size.aspect_ratio;
    }

    if (!computed_aspect_ratio.preferred_ratio.has_value())
        return {};

    auto ratio = computed_aspect_ratio.preferred_ratio.value();
    if (ratio.is_degenerate())
        return {};

    auto fraction = CSSPixelFraction(ratio.numerator(), ratio.denominator());
    // ratio.is_degenerate() operates on doubles while CSSPixelFraction uses CSSPixels, so we need to check again here.
    if (fraction == 0)
        return {};

    return fraction;
}

}
