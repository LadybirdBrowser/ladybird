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

GC_DEFINE_ALLOCATOR(Box);

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

CSS::SizeWithAspectRatio Box::auto_content_box_size() const
{
    // https://drafts.csswg.org/css-contain-2/#containment-size
    // Replaced elements must be treated as having a natural width and height of 0 and no natural aspect
    // ratio.
    if (has_size_containment())
        return { 0, 0, {} };

    return compute_auto_content_box_size();
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

bool Box::can_be_partial_relayout_boundary() const
{
    if (is_svg_svg_box())
        return true;

    if (!is_absolutely_positioned())
        return false;

    auto const& computed_values = this->computed_values();

    // Must clip overflow (not visible) — prevents stale ancestor overflow rects.
    if (computed_values.overflow_x() == CSS::Overflow::Visible || computed_values.overflow_y() == CSS::Overflow::Visible)
        return false;

    auto const& inset = computed_values.inset();
    auto is_fixed_inset = [](CSS::LengthPercentageOrAuto const& v) {
        return !v.is_auto() && v.is_length();
    };

    // Width: must be a fixed length, OR auto with both left+right being fixed insets.
    // When auto + fixed insets, width = CB_width - left - right - margins - padding - borders,
    // which is stable during partial relayout since the CB doesn't change.
    bool has_deterministic_width = computed_values.width().is_length()
        || (computed_values.width().is_auto() && is_fixed_inset(inset.left()) && is_fixed_inset(inset.right()));
    bool has_deterministic_height = computed_values.height().is_length()
        || (computed_values.height().is_auto() && is_fixed_inset(inset.top()) && is_fixed_inset(inset.bottom()));
    if (!has_deterministic_width || !has_deterministic_height)
        return false;

    // Min/max sizes must not contain percentages or be intrinsic.
    for (auto const* size : { &computed_values.min_width(), &computed_values.max_width(), &computed_values.min_height(), &computed_values.max_height() }) {
        if (size->contains_percentage() || size->is_intrinsic_sizing_constraint())
            return false;
    }

    // Padding must not contain percentages.
    auto const& padding = computed_values.padding();
    if (padding.top().contains_percentage() || padding.right().contains_percentage() || padding.bottom().contains_percentage() || padding.left().contains_percentage())
        return false;

    // Margin must not contain percentages.
    auto const& margin = computed_values.margin();
    if (margin.top().contains_percentage() || margin.right().contains_percentage() || margin.bottom().contains_percentage() || margin.left().contains_percentage())
        return false;

    return true;
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
