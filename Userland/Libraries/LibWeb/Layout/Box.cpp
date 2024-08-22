/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
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

Box::Box(DOM::Document& document, DOM::Node* node, NonnullRefPtr<CSS::StyleProperties> style)
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

// https://www.w3.org/TR/css-overflow-3/#overflow-control
static bool overflow_value_makes_box_a_scroll_container(CSS::Overflow overflow)
{
    switch (overflow) {
    case CSS::Overflow::Clip:
    case CSS::Overflow::Visible:
        return false;
    case CSS::Overflow::Auto:
    case CSS::Overflow::Hidden:
    case CSS::Overflow::Scroll:
        return true;
    }
    VERIFY_NOT_REACHED();
}

// https://www.w3.org/TR/css-overflow-3/#scroll-container
bool Box::is_scroll_container() const
{
    // NOTE: This isn't in the spec, but we want the viewport to behave like a scroll container.
    if (is_viewport())
        return true;

    return overflow_value_makes_box_a_scroll_container(computed_values().overflow_x())
        || overflow_value_makes_box_a_scroll_container(computed_values().overflow_y());
}

bool Box::is_user_scrollable() const
{
    // FIXME: Support horizontal scroll as well (overflow-x)
    return computed_values().overflow_y() == CSS::Overflow::Scroll || computed_values().overflow_y() == CSS::Overflow::Auto;
}

bool Box::is_body() const
{
    return dom_node() && dom_node() == document().body();
}

JS::GCPtr<Painting::Paintable> Box::create_paintable() const
{
    return Painting::PaintableBox::create(*this);
}

Painting::PaintableBox* Box::paintable_box()
{
    return static_cast<Painting::PaintableBox*>(Node::paintable());
}

Painting::PaintableBox const* Box::paintable_box() const
{
    return static_cast<Painting::PaintableBox const*>(Node::paintable());
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
