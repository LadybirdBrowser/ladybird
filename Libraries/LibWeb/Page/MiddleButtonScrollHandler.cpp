/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Page/AutoScrollHandler.h>
#include <LibWeb/Page/MiddleButtonScrollHandler.h>
#include <LibWeb/Painting/BoxViews.h>

namespace Web {

static constexpr double DEAD_ZONE_RADIUS { 15 };
static constexpr double SPEED_FACTOR = 5.0;
static constexpr double MAX_SPEED_PER_SECOND = 5000.0;
static constexpr double SCROLL_INTERVAL_MS = 16.0;

MiddleButtonScrollHandler::MiddleButtonScrollHandler(DOM::Element& container, CSSPixelPoint origin)
    : m_container_element(container)
    , m_origin(origin)
    , m_mouse_position(origin)
{
    auto const* layout_node = m_container_element->document().layout_node();
    if (layout_node && Painting::has_committed_box(*layout_node))
        Painting::set_needs_repaint(*layout_node);
}

MiddleButtonScrollHandler::~MiddleButtonScrollHandler()
{
    if (!m_container_element->document().layout_is_up_to_date())
        return;
    auto const* layout_node = m_container_element->document().layout_node();
    if (layout_node && Painting::has_committed_box(*layout_node))
        Painting::set_needs_repaint(*layout_node);
}

void MiddleButtonScrollHandler::visit_edges(JS::Cell::Visitor& visitor) const
{
    visitor.visit(m_container_element);
}

GC::Ptr<DOM::Element> MiddleButtonScrollHandler::find_scrollable_ancestor(DOM::Document& document, Layout::Node& layout_node)
{
    // AutoScrollHandler::find_scrollable_ancestor begins with the node's containing block. For middle mouse
    // scrolling, we want to include the node itself. This allows clicking in dead space to begin scrolling.
    if (Painting::could_be_scrolled_by_wheel_event(layout_node)) {
        if (auto* element = as_if<DOM::Element>(layout_node.dom_node()))
            return element;
    }

    if (auto* containing_block = layout_node.containing_block(); containing_block) {
        if (auto container = AutoScrollHandler::find_scrollable_ancestor(*containing_block))
            return container;
    }

    if (auto scrolling_element = document.scrolling_element())
        return const_cast<DOM::Element*>(scrolling_element.ptr());

    return {};
}

void MiddleButtonScrollHandler::perform_tick()
{
    auto distance_x = (m_mouse_position.x() - m_origin.x()).to_double();
    auto distance_y = (m_mouse_position.y() - m_origin.y()).to_double();

    if (auto distance = AK::hypot(distance_x, distance_y); distance < DEAD_ZONE_RADIUS)
        return;

    m_container_element->document().update_layout(DOM::UpdateLayoutReason::AutoScrollSelection);
    m_mouse_has_moved_beyond_dead_zone = true;

    auto* layout_node = AutoScrollHandler::auto_scroll_layout_node(m_container_element);
    if (!layout_node)
        return;

    auto speed_x = clamp(distance_x * SPEED_FACTOR, -MAX_SPEED_PER_SECOND, MAX_SPEED_PER_SECOND);
    auto speed_y = clamp(distance_y * SPEED_FACTOR, -MAX_SPEED_PER_SECOND, MAX_SPEED_PER_SECOND);
    auto elapsed_seconds = SCROLL_INTERVAL_MS / 1000.0;

    m_fractional_delta += CSSPixelPoint {
        CSSPixels(speed_x * elapsed_seconds),
        CSSPixels(speed_y * elapsed_seconds),
    };

    auto scroll_x = m_fractional_delta.x().to_int();
    auto scroll_y = m_fractional_delta.y().to_int();
    m_fractional_delta -= CSSPixelPoint { scroll_x, scroll_y };

    Painting::scroll_by(*layout_node, scroll_x, scroll_y);
}

}
