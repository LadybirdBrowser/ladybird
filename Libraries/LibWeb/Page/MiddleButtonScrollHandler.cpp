/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Page/AutoScrollHandler.h>
#include <LibWeb/Page/MiddleButtonScrollHandler.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/ViewportPaintable.h>

namespace Web {

static constexpr CSSPixels DEAD_ZONE_RADIUS { 15 };
static constexpr double SPEED_FACTOR = 5.0;
static constexpr double MAX_SPEED_PER_SECOND = 5000.0;
static constexpr double SCROLL_INTERVAL_MS = 16.0;

MiddleButtonScrollHandler::MiddleButtonScrollHandler(DOM::Element& container, CSSPixelPoint origin)
    : m_container_element(container)
    , m_origin(origin)
    , m_mouse_position(origin)
{
    if (auto* paintable = m_container_element->document().paintable())
        paintable->set_needs_repaint();
}

MiddleButtonScrollHandler::~MiddleButtonScrollHandler()
{
    if (auto* paintable = m_container_element->document().paintable())
        paintable->set_needs_repaint();
}

void MiddleButtonScrollHandler::visit_edges(JS::Cell::Visitor& visitor) const
{
    visitor.visit(m_container_element);
}

GC::Ptr<DOM::Element> MiddleButtonScrollHandler::find_scrollable_ancestor(DOM::Document& document, Painting::Paintable& paintable)
{
    // AutoScrollHandler::find_scrollable_ancestor begins with the paintable's containing block. For middle mouse
    // scrolling, we want to include the paintable itself. This allows clicking in dead space to being scrolling.
    if (auto* paintable_box = as_if<Painting::PaintableBox>(paintable); paintable_box && paintable_box->could_be_scrolled_by_wheel_event()) {
        if (auto* element = as_if<DOM::Element>(paintable_box->dom_node().ptr()))
            return element;
    }

    if (auto container = AutoScrollHandler::find_scrollable_ancestor(paintable))
        return container;

    if (auto scrolling_element = document.scrolling_element())
        return const_cast<DOM::Element*>(scrolling_element.ptr());

    return {};
}

void MiddleButtonScrollHandler::update_mouse_position(CSSPixelPoint position)
{
    m_mouse_position = position;
    m_mouse_has_moved = true;
}

void MiddleButtonScrollHandler::perform_tick()
{
    auto distance_x = (m_mouse_position.x() - m_origin.x()).to_double();
    auto distance_y = (m_mouse_position.y() - m_origin.y()).to_double();

    if (auto distance = AK::hypot(distance_x, distance_y); distance < DEAD_ZONE_RADIUS.to_double())
        return;

    m_container_element->document().update_layout(DOM::UpdateLayoutReason::AutoScrollSelection);

    auto paintable_box = AutoScrollHandler::auto_scroll_paintable(m_container_element);
    if (!paintable_box)
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

    paintable_box->scroll_by(scroll_x, scroll_y);
}

}
