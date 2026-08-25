/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLBodyElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/TextOffsetMapping.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/DisplayListCommand.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
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
    return Layout::RustFFI::layout_arena_paintable_minimum_scroll_offset(node.arena_handle(), committed_row_slot(node));
}

CSSPixelPoint maximum_scroll_offset(Layout::Node const& node)
{
    return Layout::RustFFI::layout_arena_paintable_maximum_scroll_offset(node.arena_handle(), committed_row_slot(node));
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

CSS::Overflow overflow_value_applied_to_viewport_for_wheel_scrolling(DOM::Document const& document, ScrollDirection direction)
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

WheelScrollableAxes wheel_scrollable_axes(Layout::Node const& node)
{
    auto overflow_x = overflow_value_applied_to_viewport_for_wheel_scrolling(node.document(), ScrollDirection::Horizontal);
    auto overflow_y = overflow_value_applied_to_viewport_for_wheel_scrolling(node.document(), ScrollDirection::Vertical);
    auto axes = Layout::RustFFI::layout_arena_paintable_wheel_scrollable_axes(
        node.arena_handle(), committed_row_slot(node), to_underlying(overflow_x), to_underlying(overflow_y));
    return { axes.horizontal, axes.vertical };
}

bool could_be_scrolled_by_wheel_event(Layout::Node const& node, ScrollDirection direction)
{
    auto axes = wheel_scrollable_axes(node);
    return direction == ScrollDirection::Horizontal ? axes.horizontal : axes.vertical;
}

bool could_be_scrolled_by_wheel_event(Layout::Node const& node)
{
    auto axes = wheel_scrollable_axes(node);
    return axes.horizontal || axes.vertical;
}

Layout::Node const* nearest_scrollable_ancestor(Layout::Node const& node)
{
    for (auto const* box = node.containing_block(); box; box = box->containing_block()) {
        if (!has_committed_box(*box))
            return nullptr;
        if (could_be_scrolled_by_wheel_event(*box))
            return box;
        if (is_fixed_position(*box))
            return nullptr;
    }
    return nullptr;
}

static GC::Ptr<DOM::EventTarget> scroll_event_target(Layout::Node& node)
{
    if (node.generated_for_pseudo_element().has_value())
        return node.pseudo_element_generator();
    return node.dom_node();
}

ScrollHandled set_scroll_offset(Layout::Node& node, CSSPixelPoint offset)
{
    if (!has_committed_box(node))
        return ScrollHandled::No;

    if (!Painting::scrollable_overflow_rect(node).has_value())
        return ScrollHandled::No;

    offset = clamp_scroll_offset(node, offset);

    if (scroll_offset(node) == offset)
        return ScrollHandled::No;

    if (node.is_viewport()) {
        auto navigable = node.document().navigable();
        VERIFY(navigable);
        navigable->perform_scroll_of_viewport_scrolling_box(offset);
        return ScrollHandled::Yes;
    }

    node.document().set_needs_to_refresh_scroll_state(true);

    if (auto pseudo_element = node.generated_for_pseudo_element(); pseudo_element.has_value()) {
        node.pseudo_element_generator()->set_scroll_offset(*pseudo_element, offset);
    } else if (auto* element = as_if<DOM::Element>(node.dom_node())) {
        element->set_scroll_offset({}, offset);
    } else {
        return ScrollHandled::No;
    }

    // https://drafts.csswg.org/cssom-view-1/#scrolling-events
    // Whenever an element gets scrolled (whether in response to user interaction or by an API),
    // the user agent must run these steps:

    // 1. Let doc be the element’s node document.
    auto& document = node.document();

    // FIXME: 2. If the element is a snap container, run the steps to update snapchanging targets for the element with
    //           the element’s eventual snap target in the block axis as newBlockTarget and the element’s eventual snap
    //           target in the inline axis as newInlineTarget.

    auto event_target = scroll_event_target(node);
    if (!event_target)
        return ScrollHandled::Yes;

    // 3. If (element, "scroll") is already in doc’s pending scroll events, abort these steps.
    // 4. Append (element, "scroll") to doc’s pending scroll events.
    if (!document.append_pending_scroll_event({ *event_target, HTML::EventNames::scroll }))
        return ScrollHandled::Yes;

    set_needs_repaint(node, InvalidateDisplayList::No);
    return ScrollHandled::Yes;
}

ScrollHandled scroll_by(Layout::Node& node, double delta_x, double delta_y)
{
    if (!has_committed_box(node))
        return ScrollHandled::No;
    return set_scroll_offset_from_user_input(node, scroll_offset(node).translated(CSSPixels::nearest_value_for(delta_x), CSSPixels::nearest_value_for(delta_y)));
}

static Optional<CompositorScrollNodeKind> scroll_node_kind_for(Layout::Node const& node)
{
    if (node.is_viewport())
        return CompositorScrollNodeKind::Viewport;
    if (node.generated_for_pseudo_element().has_value())
        return CompositorScrollNodeKind::PseudoElement;
    if (node.dom_node() && is<DOM::Element>(*node.dom_node()))
        return CompositorScrollNodeKind::Element;
    return {};
}

static UniqueNodeID scrollable_node_id_for(Layout::Node const& node)
{
    if (node.is_viewport())
        return node.document().unique_id();
    if (node.generated_for_pseudo_element().has_value())
        return node.pseudo_element_generator()->unique_id();
    return node.dom_node()->unique_id();
}

static u8 pseudo_element_type_for(Layout::Node const& node)
{
    auto pseudo_element = node.generated_for_pseudo_element();
    if (!pseudo_element.has_value())
        return 0;
    return static_cast<u8>(to_underlying(*pseudo_element));
}

Optional<Compositor::AsyncScrollNodeStableID> async_scroll_node_stable_id(Layout::Node const& node)
{
    auto scroll_node_kind = scroll_node_kind_for(node);
    if (!scroll_node_kind.has_value())
        return {};

    return Compositor::AsyncScrollNodeStableID {
        .node_id = scrollable_node_id_for(node),
        .kind = Compositor::async_scroll_node_kind_for(*scroll_node_kind),
        .pseudo_element_type = pseudo_element_type_for(node),
    };
}

ScrollHandled set_scroll_offset_from_user_input(Layout::Node& node, CSSPixelPoint offset)
{
    if (!has_committed_box(node))
        return ScrollHandled::No;

    auto navigable = node.document().navigable();
    auto stable_node_id = async_scroll_node_stable_id(node);

    auto scroll_offset_before_scroll = scroll_offset(node);

    if (navigable && stable_node_id.has_value())
        navigable->abort_in_flight_smooth_scrolls_taken_over_by_user_input(*stable_node_id, scroll_offset_before_scroll);

    auto scroll_handled = set_scroll_offset(node, offset);
    if (!navigable)
        return scroll_handled;

    if (scroll_handled == ScrollHandled::Yes) {
        if (auto event_target = scroll_event_target(node))
            navigable->queue_scrollend_event_after_user_scroll(*event_target, stable_node_id, scroll_offset_before_scroll);
    } else {
        // User input keeps the scroll gesture in progress even when it does not move the scrolling box.
        navigable->defer_user_scroll_settlement();
    }
    return scroll_handled;
}

ScrollHandled wheel_scroll(Layout::Node& node, double wheel_delta_x, double wheel_delta_y)
{
    if (node.is_viewport())
        return ScrollHandled::No;
    auto axes = wheel_scrollable_axes(node);
    if (!axes.horizontal)
        wheel_delta_x = 0;
    if (!axes.vertical)
        wheel_delta_y = 0;
    if (wheel_delta_x == 0 && wheel_delta_y == 0)
        return ScrollHandled::No;
    return scroll_by(node, wheel_delta_x, wheel_delta_y);
}

ScrollHandled wheel_scroll_along_containing_block_chain(Layout::Node& node, double wheel_delta_x, double wheel_delta_y)
{
    for (auto* current = &node; current; current = current->containing_block()) {
        if (wheel_scroll(*current, wheel_delta_x, wheel_delta_y) == ScrollHandled::Yes)
            return ScrollHandled::Yes;
    }
    return ScrollHandled::No;
}

static void scroll_into_view(Layout::Node& node, CSSPixelRect rect)
{
    if (!has_committed_box(node))
        return;

    auto snapport = scroll_snapport_rect(node);
    auto current_offset = scroll_offset(node);

    // Both rect and snapport are in layout coordinate space (not scroll-adjusted).
    auto content_rect = rect.translated(-snapport.x(), -snapport.y());
    auto new_offset = current_offset;

    if (content_rect.right() > current_offset.x() + snapport.width())
        new_offset.set_x(content_rect.right() - snapport.width());
    else if (content_rect.left() < current_offset.x())
        new_offset.set_x(content_rect.left());

    if (content_rect.bottom() > current_offset.y() + snapport.height())
        new_offset.set_y(content_rect.bottom() - snapport.height());
    else if (content_rect.top() < current_offset.y())
        new_offset.set_y(content_rect.top());

    set_scroll_offset(node, new_offset);
}

void scroll_text_offset_into_view(DOM::Text const& text, size_t offset, TextAffinity affinity, ScrollBlockDirection scroll_block_direction)
{
    auto text_slots = Layout::TextOffsetMapping { text }.slot_ids();
    if (text_slots.is_empty())
        return;

    auto const* layout_node = text.unsafe_layout_node();
    auto result = Layout::RustFFI::layout_arena_text_caret_rect_for_position(
        layout_node->arena_handle(), text_slots.data(), text_slots.size(), offset,
        affinity == TextAffinity::Downstream);
    if (!result.found)
        return;
    auto const* style_source_pointer = static_cast<Layout::NodeWithStyle const*>(result.style_source);
    if (!style_source_pointer)
        return;
    auto const& style_source = *style_source_pointer;

    auto cursor_rect = result.rect;
    if (style_source.writing_mode() == CSS::WritingMode::HorizontalTb) {
        if (style_source.inline_axis_is_reverse())
            cursor_rect.set_x(cursor_rect.x() - 1);
        cursor_rect.set_width(1);
    } else {
        if (style_source.inline_axis_is_reverse())
            cursor_rect.set_y(cursor_rect.y() - 1);
        cursor_rect.set_height(1);
    }
    auto* owner = layout_node_for_committed_slot(layout_node->node_arena(), result.owner_paintable);
    for (auto* ancestor = owner; ancestor;) {
        if (Painting::has_scrollable_overflow(*ancestor)) {
            if (scroll_block_direction == ScrollBlockDirection::No) {
                auto snapport = scroll_snapport_rect(*ancestor);
                if (style_source.writing_mode() == CSS::WritingMode::HorizontalTb) {
                    cursor_rect.set_y(snapport.y() + scroll_offset(*ancestor).y());
                    cursor_rect.set_height(snapport.height());
                } else {
                    cursor_rect.set_x(snapport.x() + scroll_offset(*ancestor).x());
                    cursor_rect.set_width(snapport.width());
                }
            }
            scroll_into_view(*ancestor, cursor_rect);
            return;
        }
        auto* containing_block_box = ancestor->containing_block();
        ancestor = containing_block_box && has_committed_box(*containing_block_box) ? containing_block_box : nullptr;
    }
}

}
