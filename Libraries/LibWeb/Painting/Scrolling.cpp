/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/HTMLBodyElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Painting/Scrolling.h>

namespace Web::Painting {

CSSPixelPoint scroll_offset(Layout::Node const& node)
{
    if (!has_committed_box(node))
        return {};

    if (node.is_viewport()) {
        auto navigable = node.document().navigable();
        VERIFY(navigable);
        return navigable->viewport_scroll_offset();
    }

    if (auto pseudo_element = node.generated_for_pseudo_element(); pseudo_element.has_value())
        return node.pseudo_element_generator()->scroll_offset(*pseudo_element);

    if (auto const* element = as_if<DOM::Element>(node.dom_node()))
        return element->scroll_offset({});
    return {};
}

CSSPixelPoint minimum_scroll_offset(Layout::Node const& node)
{
    auto scrollable_overflow_rect = Painting::scrollable_overflow_rect(node);
    if (!scrollable_overflow_rect.has_value())
        return {};

    auto scrollport_rect = absolute_padding_box_rect(node);
    return {
        min(scrollable_overflow_rect->left() - scrollport_rect.left(), CSSPixels(0)),
        min(scrollable_overflow_rect->top() - scrollport_rect.top(), CSSPixels(0)),
    };
}

CSSPixelPoint maximum_scroll_offset(Layout::Node const& node)
{
    auto scrollable_overflow_rect = Painting::scrollable_overflow_rect(node);
    if (!scrollable_overflow_rect.has_value())
        return {};

    auto scrollport_rect = absolute_padding_box_rect(node);
    return {
        max(scrollable_overflow_rect->right() - scrollport_rect.right(), CSSPixels(0)),
        max(scrollable_overflow_rect->bottom() - scrollport_rect.bottom(), CSSPixels(0)),
    };
}

CSSPixelPoint clamp_scroll_offset(Layout::Node const& node, CSSPixelPoint offset)
{
    if (!Painting::scrollable_overflow_rect(node).has_value())
        return offset;

    auto minimum_offset = minimum_scroll_offset(node);
    auto maximum_offset = maximum_scroll_offset(node);
    return {
        clamp(offset.x(), minimum_offset.x(), maximum_offset.x()),
        clamp(offset.y(), minimum_offset.y(), maximum_offset.y()),
    };
}

CSSPixelRect scroll_snapport_rect(Layout::Node const& node)
{
    if (!has_committed_box(node))
        return {};
    return scroll_snapport_rect(node, absolute_padding_box_rect(node));
}

CSSPixelRect scroll_snapport_rect(Layout::Node const& node, CSSPixelRect scrollport)
{
    if (!has_committed_box(node))
        return scrollport;

    auto const& node_with_style = as<Layout::NodeWithStyle>(node);
    Layout::NodeWithStyle const* scroll_padding_source = &node_with_style;

    if (node.is_viewport()) {
        auto const* document_element = node.document().document_element();
        auto const* document_element_layout_node = document_element ? document_element->unsafe_layout_node() : nullptr;
        if (!document_element_layout_node)
            return scrollport;
        scroll_padding_source = document_element_layout_node;
    }

    // Percentages refer to the corresponding dimension of the scroll container’s scrollport.
    auto const& scroll_padding = scroll_padding_source->scroll_padding();
    scrollport.shrink(
        scroll_padding.top().to_px_or_zero(scrollport.height()),
        scroll_padding.right().to_px_or_zero(scrollport.width()),
        scroll_padding.bottom().to_px_or_zero(scrollport.height()),
        scroll_padding.left().to_px_or_zero(scrollport.width()));
    return scrollport;
}

static CSS::Overflow overflow_value_applied_to_viewport_for_wheel_scrolling(DOM::Document const& document, ScrollDirection direction)
{
    auto overflow_for_direction = [direction](CSS::ComputedValues::BoxValues const& style) {
        return direction == ScrollDirection::Horizontal
            ? static_cast<CSS::Overflow>(style.overflow_x)
            : static_cast<CSS::Overflow>(style.overflow_y);
    };
    auto has_containment = [](CSS::ComputedValues::BoxValues const& style) {
        return style.size_containment || style.inline_size_containment || style.layout_containment || style.style_containment || style.paint_containment;
    };

    auto* root_element = document.document_element();
    auto const* root_style = root_element ? root_element->style_group<CSS::ComputedValues::BoxValues>() : nullptr;
    if (!root_style)
        return CSS::Overflow::Auto;

    auto const* overflow_origin = root_style;
    if (root_element->is_html_html_element() && !has_containment(*root_style)) {
        auto root_overflow_x = static_cast<CSS::Overflow>(root_style->overflow_x);
        auto root_overflow_y = static_cast<CSS::Overflow>(root_style->overflow_y);
        if (root_overflow_x == CSS::Overflow::Visible && root_overflow_y == CSS::Overflow::Visible) {
            auto* body_element = root_element->first_child_of_type<HTML::HTMLBodyElement>();
            auto const* body_style = body_element ? body_element->style_group<CSS::ComputedValues::BoxValues>() : nullptr;
            if (body_style && !has_containment(*body_style))
                overflow_origin = body_style;
        }
    }

    auto overflow = overflow_for_direction(*overflow_origin);
    if (overflow == CSS::Overflow::Visible)
        return CSS::Overflow::Auto;
    if (overflow == CSS::Overflow::Clip)
        return CSS::Overflow::Hidden;
    return overflow;
}

bool could_be_scrolled_by_wheel_event(Layout::Node const& node, ScrollDirection direction)
{
    if (!has_committed_box(node))
        return false;

    auto const& node_with_style = as<Layout::NodeWithStyle>(node);
    bool is_horizontal = direction == ScrollDirection::Horizontal;
    Gfx::Orientation orientation = is_horizontal ? Gfx::Orientation::Horizontal : Gfx::Orientation::Vertical;
    auto overflow = is_horizontal ? node_with_style.overflow_x() : node_with_style.overflow_y();
    if (node.is_viewport())
        overflow = overflow_value_applied_to_viewport_for_wheel_scrolling(node.document(), direction);

    if (overflow != CSS::Overflow::Auto && overflow != CSS::Overflow::Scroll)
        return false;

    auto scrollable_overflow_rect = Painting::scrollable_overflow_rect(node);
    if (!scrollable_overflow_rect.has_value())
        return false;

    CSSPixels scrollable_overflow_size = scrollable_overflow_rect->primary_size_for_orientation(orientation);
    CSSPixels scrollport_size = absolute_padding_box_rect(node).primary_size_for_orientation(orientation);

    return scrollable_overflow_size > scrollport_size;
}

bool could_be_scrolled_by_wheel_event(Layout::Node const& node)
{
    return could_be_scrolled_by_wheel_event(node, ScrollDirection::Horizontal) || could_be_scrolled_by_wheel_event(node, ScrollDirection::Vertical);
}

RefPtr<Paintable const> nearest_scrollable_ancestor(Layout::Node const& node)
{
    for (auto const* box = node.containing_block(); box; box = box->containing_block()) {
        auto const* paintable = box->paintable_ptr();
        if (!paintable)
            return nullptr;
        if (could_be_scrolled_by_wheel_event(*box))
            return paintable;
        if (is_fixed_position(*box))
            return nullptr;
    }
    return nullptr;
}

}
