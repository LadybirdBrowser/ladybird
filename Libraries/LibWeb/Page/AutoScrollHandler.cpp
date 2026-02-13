/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/Page/AutoScrollHandler.h>
#include <LibWeb/Page/EventHandler.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/ViewportPaintable.h>

namespace Web {

static constexpr int auto_scroll_interval_ms = 16;
static constexpr CSSPixels auto_scroll_edge_threshold { 7 };
static constexpr CSSPixels viewport_edge_inset { 25 };

// Returns the scrollport shrunk by per-side effective auto scroll edge thresholds. Sides close to a viewport edge get
// a larger inset so the distance-based speed ramp works predictably, even if the user's mouse is limited in reach (e.g.
// by the window/screen boundary).
static CSSPixelRect compute_effective_auto_scroll_edge(CSSPixelRect const& scrollport, CSSPixelRect const& viewport_rect)
{
    auto effective = [](CSSPixels distance_to_viewport_edge) {
        return auto_scroll_edge_threshold + max(CSSPixels(0), viewport_edge_inset - distance_to_viewport_edge);
    };
    return scrollport.shrunken(
        effective(scrollport.top() - viewport_rect.top()),
        effective(viewport_rect.right() - scrollport.right()),
        effective(viewport_rect.bottom() - scrollport.bottom()),
        effective(scrollport.left() - viewport_rect.left()));
}

static Optional<CSSPixelRect> scrollport_rect_in_viewport(Painting::PaintableBox const& paintable_box)
{
    auto scrollport = paintable_box.absolute_padding_box_rect();

    // The viewport's scrollport is already in viewport coordinates.
    if (paintable_box.is_viewport_paintable())
        return scrollport;

    auto const& accumulated_visual_context = paintable_box.accumulated_visual_context();
    if (!accumulated_visual_context)
        return {};
    auto const* viewport_paintable = paintable_box.document().paintable();
    if (!viewport_paintable)
        return {};
    auto const& scroll_state = viewport_paintable->scroll_state_snapshot();
    return accumulated_visual_context->transform_rect_to_viewport(scrollport, scroll_state);
}

// Returns scroll speed in CSS pixels per second for each axis, based on how far the mouse is past the auto scroll edge.
static CSSPixelPoint compute_auto_scroll_speed(CSSPixelPoint mouse, CSSPixelRect const& edge)
{
    static CSSPixels constexpr distance_to_speed_factor { 100 };
    static CSSPixels constexpr max_speed_per_second { 5000 };

    auto compute_axis_speed = [&](CSSPixels mouse_pos, CSSPixels edge_start, CSSPixels edge_end) -> CSSPixels {
        if (mouse_pos < edge_start) {
            auto distance = edge_start - mouse_pos;
            return -min(distance * distance_to_speed_factor, max_speed_per_second);
        }
        if (mouse_pos > edge_end) {
            auto distance = mouse_pos - edge_end;
            return min(distance * distance_to_speed_factor, max_speed_per_second);
        }
        return 0;
    };

    return {
        compute_axis_speed(mouse.x(), edge.x(), edge.x() + edge.width()),
        compute_axis_speed(mouse.y(), edge.y(), edge.y() + edge.height()),
    };
}

static bool is_in_form_associated_text_control(DOM::Element const& element)
{
    auto const& host = element.containing_shadow_root() ? *element.containing_shadow_root()->host() : element;
    return is<HTML::FormAssociatedTextControlElement>(host);
}

// Returns the paintable box that manages the scrollport for an auto-scroll container element. When the element is the
// document's scrolling element, the viewport paintable is the scroll container.
static Painting::PaintableBox* auto_scroll_paintable(DOM::Element& element)
{
    if (element.document().scrolling_element().ptr() == &element)
        return element.document().paintable();
    return element.paintable_box();
}

AutoScrollHandler::AutoScrollHandler(HTML::Navigable& navigable, DOM::Element& container)
    : m_navigable(navigable)
    , m_container_element(container)
{
}

void AutoScrollHandler::visit_edges(JS::Cell::Visitor& visitor) const
{
    visitor.visit(m_navigable);
    visitor.visit(m_container_element);
}

CSSPixelPoint AutoScrollHandler::process(CSSPixelPoint mouse_position)
{
    m_mouse_position = mouse_position;

    auto* paintable_box = auto_scroll_paintable(m_container_element);
    if (!paintable_box)
        return mouse_position;

    auto scrollport = scrollport_rect_in_viewport(*paintable_box);
    if (!scrollport.has_value())
        return mouse_position;

    CSSPixelRect viewport_rect { {}, m_navigable->viewport_size() };
    auto effective_edge = compute_effective_auto_scroll_edge(*scrollport, viewport_rect);
    if (effective_edge.contains(mouse_position)) {
        deactivate();
        return mouse_position;
    }

    activate();
    if (is_in_form_associated_text_control(m_container_element))
        return constrained(mouse_position, *scrollport);
    return mouse_position;
}

GC::Ptr<DOM::Element> AutoScrollHandler::find_scrollable_ancestor(Painting::Paintable const& paintable)
{
    auto* paintable_box = paintable.containing_block();
    while (paintable_box) {
        if (paintable_box->has_scrollable_overflow()) {
            if (auto* element = as_if<DOM::Element>(paintable_box->dom_node().ptr()))
                return element;
        }

        // The viewport is always a potential scroll container, but may not report has_scrollable_overflow() and its DOM
        // node is Document (not Element).
        if (paintable_box->is_viewport_paintable() && paintable_box->could_be_scrolled_by_wheel_event()) {
            if (auto scrolling_element = paintable_box->document().scrolling_element())
                return const_cast<DOM::Element*>(scrolling_element.ptr());
        }

        paintable_box = paintable_box->containing_block();
    }
    return {};
}

void AutoScrollHandler::activate()
{
    m_active = true;
}

void AutoScrollHandler::deactivate()
{
    m_active = false;
    m_fractional_delta = {};
}

void AutoScrollHandler::perform_tick()
{
    if (!m_active)
        return;

    if (!m_navigable->event_handler().is_handling_mouse_selection()) {
        deactivate();
        return;
    }

    auto& document = *m_navigable->active_document();
    document.update_layout(DOM::UpdateLayoutReason::AutoScrollSelection);

    auto* paintable_box = auto_scroll_paintable(m_container_element);
    if (!paintable_box || !document.paintable()) {
        deactivate();
        return;
    }

    auto scrollport = scrollport_rect_in_viewport(*paintable_box);
    if (!scrollport.has_value()) {
        deactivate();
        return;
    }

    CSSPixelRect viewport_rect { {}, m_navigable->viewport_size() };
    auto effective_edge = compute_effective_auto_scroll_edge(*scrollport, viewport_rect);
    if (effective_edge.contains(m_mouse_position)) {
        deactivate();
        return;
    }

    auto elapsed_seconds = static_cast<double>(auto_scroll_interval_ms) / 1000.0;
    auto speed = compute_auto_scroll_speed(m_mouse_position, effective_edge);

    // Accumulate sub-pixel deltas across ticks, since scroll_by() only accepts whole pixels.
    m_fractional_delta += CSSPixelPoint {
        speed.x() * CSSPixels(elapsed_seconds),
        speed.y() * CSSPixels(elapsed_seconds),
    };
    int scroll_x = m_fractional_delta.x().to_int();
    int scroll_y = m_fractional_delta.y().to_int();
    m_fractional_delta -= CSSPixelPoint { scroll_x, scroll_y };

    if (paintable_box->scroll_by(scroll_x, scroll_y) == Painting::PaintableBox::ScrollHandled::No)
        return;

    auto selection_position = is_in_form_associated_text_control(m_container_element)
        ? constrained(m_mouse_position, *scrollport)
        : m_mouse_position;
    m_navigable->event_handler().apply_mouse_selection(selection_position);
}

}
