/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Page/AutoScrollHandler.h>
#include <LibWeb/Page/MiddleButtonScrollHandler.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/Scrolling.h>

namespace Web {

static constexpr double DEAD_ZONE_RADIUS { 15 };
static constexpr double SPEED_FACTOR = 5.0;
static constexpr double MAX_SPEED_PER_SECOND = 5000.0;
static constexpr double MAX_FRAME_INTERVAL_MS = 100.0;

MiddleButtonScrollHandler::MiddleButtonScrollHandler(DOM::Element& container, CSSPixelPoint origin, CSSPixelPoint screen_origin)
    : m_container_element(container)
    , m_origin(origin)
    , m_screen_origin(screen_origin)
    , m_mouse_screen_position(screen_origin)
{
    request_indicator_repaint();
}

// The handler can die along with its navigable in a GC sweep, after its element and document are already gone — so
// the destructor must not touch them. Instead, EventHandler asks for the repaint when it stops the scroll.
MiddleButtonScrollHandler::~MiddleButtonScrollHandler() = default;

void MiddleButtonScrollHandler::request_indicator_repaint()
{
    static_cast<DOM::Node&>(m_container_element->document()).set_needs_repaint(InvalidateDisplayList::PaintCommands);
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

    auto const* viewport = document.layout_node(layout_node.held_read());
    auto document_can_scroll = viewport && Painting::could_be_scrolled_by_wheel_event(*viewport);

    if (auto* containing_block = layout_node.containing_block(); containing_block) {
        if (auto container = AutoScrollHandler::find_scrollable_ancestor(*containing_block); container && (document_can_scroll || container != document.scrolling_element()))
            return container;
    }

    // A nested document that can't scroll hands the scroll to the document it's nested in, from its container on.
    if (!document_can_scroll) {
        if (auto navigable = document.navigable(); navigable && navigable->container()) {
            auto& container = *navigable->container();
            auto& parent_document = container.document();
            Layout::ForcedReadScope read { parent_document };
            parent_document.update_layout(DOM::UpdateLayoutReason::EventHandlerHandleMouseDown);
            if (auto* container_layout_node = container.layout_node(read))
                return find_scrollable_ancestor(parent_document, *container_layout_node);
        }
    }

    if (auto scrolling_element = document.scrolling_element())
        return const_cast<DOM::Element*>(scrolling_element.ptr());

    return {};
}

// NB: A drag out of the dead zone counts as soon as the pointer gets there, so that releasing the button ends the
//     scroll even before a rendering update has scrolled anything.
void MiddleButtonScrollHandler::update_mouse_screen_position(CSSPixelPoint screen_position)
{
    m_mouse_screen_position = screen_position;
    auto distance_x = (m_mouse_screen_position.x() - m_screen_origin.x()).to_double();
    auto distance_y = (m_mouse_screen_position.y() - m_screen_origin.y()).to_double();
    if (AK::hypot(distance_x, distance_y) >= DEAD_ZONE_RADIUS)
        m_mouse_has_moved_beyond_dead_zone = true;
}

void MiddleButtonScrollHandler::perform_tick(double frame_timestamp)
{
    auto elapsed_ms = clamp(frame_timestamp - m_last_frame_time.value_or(frame_timestamp), 0.0, MAX_FRAME_INTERVAL_MS);
    m_last_frame_time = frame_timestamp;

    auto distance_x = (m_mouse_screen_position.x() - m_screen_origin.x()).to_double();
    auto distance_y = (m_mouse_screen_position.y() - m_screen_origin.y()).to_double();

    if (auto distance = AK::hypot(distance_x, distance_y); distance < DEAD_ZONE_RADIUS)
        return;

    Layout::ForcedReadScope read { m_container_element->document() };
    m_container_element->document().update_layout(DOM::UpdateLayoutReason::AutoScrollSelection);
    m_mouse_has_moved_beyond_dead_zone = true;

    auto* layout_node = AutoScrollHandler::auto_scroll_layout_node(read, m_container_element);
    if (!layout_node)
        return;

    auto speed_x = clamp(distance_x * SPEED_FACTOR, -MAX_SPEED_PER_SECOND, MAX_SPEED_PER_SECOND);
    auto speed_y = clamp(distance_y * SPEED_FACTOR, -MAX_SPEED_PER_SECOND, MAX_SPEED_PER_SECOND);
    auto elapsed_seconds = elapsed_ms / 1000.0;

    m_fractional_delta += CSSPixelPoint {
        CSSPixels(speed_x * elapsed_seconds),
        CSSPixels(speed_y * elapsed_seconds),
    };

    auto scroll_x = m_fractional_delta.x().to_int();
    auto scroll_y = m_fractional_delta.y().to_int();
    m_fractional_delta -= CSSPixelPoint { scroll_x, scroll_y };

    if (auto navigable = m_container_element->document().navigable()) {
        // Middle button scrolling is one scroll gesture that runs until the mode it belongs to is exited, rather than
        // until it runs out of scrolls, so it is held open for as long as this handler lives.
        if (!m_scroll_gesture_hold)
            m_scroll_gesture_hold = make<HTML::UserScrollGestureHold>(*navigable);
        if (auto stable_node_id = Painting::async_scroll_node_stable_id(*layout_node); stable_node_id.has_value())
            m_scroll_gesture_hold->hold(stable_node_id.value());
        navigable->note_user_scroll_input_intent(Compositing::SnapSelectionStrategy::Type::EndPosition);
    }
    Painting::scroll_by(*layout_node, scroll_x, scroll_y);
}

}
