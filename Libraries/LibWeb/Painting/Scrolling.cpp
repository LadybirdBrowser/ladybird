/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/DisplayList/DisplayListCommand.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/Painting/Scrolling.h>

namespace Web::Painting {

static Web::AsyncScrollNodeKind async_scroll_node_kind_for(Compositing::CompositorScrollNodeKind kind)
{
    switch (kind) {
    case Compositing::CompositorScrollNodeKind::Viewport:
        return Web::AsyncScrollNodeKind::Viewport;
    case Compositing::CompositorScrollNodeKind::Element:
        return Web::AsyncScrollNodeKind::Element;
    case Compositing::CompositorScrollNodeKind::PseudoElement:
        return Web::AsyncScrollNodeKind::PseudoElement;
    }
    VERIFY_NOT_REACHED();
}

CSSPixelPoint scroll_offset(Layout::Node const& node)
{
    if (!has_committed_box(node))
        return {};
    CSSPixelPoint offset;
    Layout::RustFFI::render_state_row_scroll_offset(node.document_host(), committed_row_slot(node), &offset);
    return offset;
}

CSSPixelPoint minimum_scroll_offset(Layout::Node const& node)
{
    CSSPixelPoint offset;
    Layout::RustFFI::layout_row_paintable_minimum_scroll_offset(node.document_host(), committed_row_slot(node), &offset);
    return offset;
}

CSSPixelPoint maximum_scroll_offset(Layout::Node const& node)
{
    CSSPixelPoint offset;
    Layout::RustFFI::layout_row_paintable_maximum_scroll_offset(node.document_host(), committed_row_slot(node), &offset);
    return offset;
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
    CSSPixelRect rect;
    Layout::RustFFI::layout_row_scroll_snapport_rect(node.document_host(), committed_row_slot(node), scrollport, &rect);
    return rect;
}

WheelScrollableAxes wheel_scrollable_axes(Layout::Node const& node)
{
    auto axes = Layout::RustFFI::layout_row_paintable_wheel_scrollable_axes(node.document_host(), committed_row_slot(node));
    return { axes.horizontal, axes.vertical };
}

bool could_be_scrolled_by_wheel_event(Layout::Node const& node)
{
    auto axes = wheel_scrollable_axes(node);
    return axes.horizontal || axes.vertical;
}

ScrollHandled set_scroll_offset(Layout::Node& node, CSSPixelPoint offset, CompositorScrollRequest compositor_scroll_request)
{
    if (!has_committed_box(node))
        return ScrollHandled::No;

    if (!Painting::scrollable_overflow_rect(node).has_value())
        return ScrollHandled::No;

    offset = clamp_scroll_offset(node, offset);

    if (scroll_offset(node) == offset)
        return ScrollHandled::No;

    auto navigable = node.document().navigable();
    auto stable_node_id = async_scroll_node_stable_id(node);
    if (!navigable || !stable_node_id.has_value())
        return ScrollHandled::No;
    return navigable->store_scroll_offset(*stable_node_id, offset, compositor_scroll_request) ? ScrollHandled::Yes : ScrollHandled::No;
}

static Optional<Compositing::CompositorScrollNodeKind> scroll_node_kind_for(Layout::Node const& node)
{
    if (node.is_viewport())
        return Compositing::CompositorScrollNodeKind::Viewport;
    if (node.generated_for_pseudo_element().has_value()) {
        if (!node.pseudo_element_generator())
            return {};
        return Compositing::CompositorScrollNodeKind::PseudoElement;
    }
    if (node.dom_node() && is<DOM::Element>(*node.dom_node()))
        return Compositing::CompositorScrollNodeKind::Element;
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

Optional<Web::AsyncScrollNodeStableID> async_scroll_node_stable_id(Layout::Node const& node)
{
    auto scroll_node_kind = scroll_node_kind_for(node);
    if (!scroll_node_kind.has_value())
        return {};

    return Web::AsyncScrollNodeStableID {
        .node_id = scrollable_node_id_for(node),
        .kind = async_scroll_node_kind_for(*scroll_node_kind),
        .pseudo_element_type = pseudo_element_type_for(node),
    };
}

ScrollHandled scroll_by(Layout::Node& node, double delta_x, double delta_y)
{
    if (!has_committed_box(node))
        return ScrollHandled::No;
    // A scroll by nothing is not user input: it neither takes over a scroll in flight nor keeps a gesture going.
    if (delta_x == 0 && delta_y == 0)
        return ScrollHandled::No;
    auto navigable = node.document().navigable();
    auto stable_node_id = async_scroll_node_stable_id(node);
    if (!navigable || !stable_node_id.has_value())
        return ScrollHandled::No;

    auto scroll_offset_before_scroll = scroll_offset(node);
    navigable->abort_in_flight_smooth_scrolls_taken_over_by_user_input(*stable_node_id, scroll_offset_before_scroll);
    auto offset = scroll_offset_before_scroll.translated(CSSPixels::nearest_value_for(delta_x), CSSPixels::nearest_value_for(delta_y));
    if (set_scroll_offset(node, offset, CompositorScrollRequest::UserInputJump) == ScrollHandled::No) {
        // User input keeps the scroll gesture in progress even when it does not move the scrolling box.
        navigable->extend_user_scroll_gesture_of(*stable_node_id);
        return ScrollHandled::No;
    }
    node.document().scroll_state_query_containers().did_scroll_relatively(node, scroll_offset(node) - scroll_offset_before_scroll);
    navigable->note_user_scroll_gesture_of(*stable_node_id, scroll_offset_before_scroll, HTML::LocalNavigable::UserScrollMovedBox::Yes);
    return ScrollHandled::Yes;
}

Layout::Node* first_wheel_scrollable_box_in_containing_block_chain(Layout::Node const& node)
{
    auto const& read = node.held_read();
    return node.node_arena().node_if_live(read, Layout::RustFFI::render_state_first_wheel_scrollable_box_in_containing_block_chain(node.document_host(), committed_row_slot(node)));
}

Layout::Node* box_ending_scroll_chain(Layout::Node const& node, double delta_x, double delta_y)
{
    auto const& read = node.held_read();
    return node.node_arena().node_if_live(read, Layout::RustFFI::render_state_box_ending_scroll_chain(node.document_host(), committed_row_slot(node), delta_x, delta_y));
}

void scroll_text_offset_into_view(Layout::BegunRead const& read, DOM::Text const& text, size_t offset, TextAffinity affinity, ScrollBlockDirection scroll_block_direction)
{
    auto const* layout_node = text.unsafe_layout_node(read);
    if (!layout_node)
        return;
    CSSPixelPoint scroll_offset;
    auto container = Layout::RustFFI::render_state_scroll_target_for_text_position(
        layout_node->document_host(), Layout::Node::slot_id(layout_node), offset,
        affinity == TextAffinity::Downstream, scroll_block_direction == ScrollBlockDirection::Yes, &scroll_offset);
    if (auto* scroll_container = layout_node_for_committed_slot(read, layout_node->node_arena(), container))
        set_scroll_offset(*scroll_container, scroll_offset, CompositorScrollRequest::ProgrammaticJump);
}

}
