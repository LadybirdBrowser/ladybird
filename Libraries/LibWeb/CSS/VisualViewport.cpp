/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/VisualViewport.h>
#include <LibWeb/CSS/VisualViewport.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/EventDispatcher.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/InvalidateDisplayList.h>
#include <LibWeb/Page/SmoothScrollHandler.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(VisualViewport);

GC::Ref<VisualViewport> VisualViewport::create(DOM::Document& document)
{
    return document.realm().create<VisualViewport>(document);
}

VisualViewport::VisualViewport(DOM::Document& document)
    : DOM::EventTarget(document.realm())
    , m_document(document)
{
}

void VisualViewport::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(VisualViewport);
    Base::initialize(realm);
}

void VisualViewport::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
}

// https://drafts.csswg.org/cssom-view/#dom-visualviewport-offsetleft
double VisualViewport::offset_left() const
{
    // 1. If the visual viewport’s associated document is not fully active, return 0.
    if (!m_document->is_fully_active())
        return 0;

    // 2. Otherwise, return the offset of the left edge of the visual viewport from the left edge of the layout viewport.
    return m_offset.x().to_double();
}

// https://drafts.csswg.org/cssom-view/#dom-visualviewport-offsettop
double VisualViewport::offset_top() const
{
    // 1. If the visual viewport’s associated document is not fully active, return 0.
    if (!m_document->is_fully_active())
        return 0;

    // 2. Otherwise, return the offset of the top edge of the visual viewport from the top edge of the layout viewport.
    return m_offset.y().to_double();
}

// https://drafts.csswg.org/cssom-view/#dom-visualviewport-pageleft
double VisualViewport::page_left() const
{
    // 1. If the visual viewport’s associated document is not fully active, return 0.
    if (!m_document->is_fully_active())
        return 0;

    // 2. Otherwise, return the offset of the left edge of the visual viewport from the
    //    left edge of the initial containing block of the layout viewport’s document.
    return m_document->viewport_rect().x().to_double() + offset_left();
}

// https://drafts.csswg.org/cssom-view/#dom-visualviewport-pagetop
double VisualViewport::page_top() const
{
    // 1. If the visual viewport’s associated document is not fully active, return 0.
    if (!m_document->is_fully_active())
        return 0;

    // 2. Otherwise, return the offset of the top edge of the visual viewport from the
    //    top edge of the initial containing block of the layout viewport’s document.
    return m_document->viewport_rect().y().to_double() + offset_top();
}

// https://drafts.csswg.org/cssom-view/#dom-visualviewport-width
double VisualViewport::width() const
{
    // 1. If the visual viewport’s associated document is not fully active, return 0.
    if (!m_document->is_fully_active())
        return 0;

    // 2. Otherwise, return the width of the visual viewport
    //    FIXME: excluding the width of any rendered vertical classic scrollbar that is fixed to the visual viewport.
    return m_document->viewport_rect().size().width() / m_scale;
}

// https://drafts.csswg.org/cssom-view/#dom-visualviewport-height
double VisualViewport::height() const
{
    // 1. If the visual viewport’s associated document is not fully active, return 0.
    if (!m_document->is_fully_active())
        return 0;

    // 2. Otherwise, return the height of the visual viewport
    //    FIXME: excluding the height of any rendered vertical classic scrollbar that is fixed to the visual viewport.
    return m_document->viewport_rect().size().height() / m_scale;
}

// https://drafts.csswg.org/cssom-view/#dom-visualviewport-scale
double VisualViewport::scale() const
{
    return m_scale;
}

void VisualViewport::set_onresize(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::resize, event_handler);
}

WebIDL::CallbackType* VisualViewport::onresize()
{
    return event_handler_attribute(HTML::EventNames::resize);
}

void VisualViewport::set_onscroll(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::scroll, event_handler);
}

WebIDL::CallbackType* VisualViewport::onscroll()
{
    return event_handler_attribute(HTML::EventNames::scroll);
}

void VisualViewport::set_onscrollend(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::scrollend, event_handler);
}

WebIDL::CallbackType* VisualViewport::onscrollend()
{
    return event_handler_attribute(HTML::EventNames::scrollend);
}

// https://drafts.csswg.org/cssom-view-1/#perform-a-scroll
GC::Ref<WebIDL::Promise> VisualViewport::perform_scroll_of_visual_viewport_scrolling_box(CSSPixelPoint offset, Bindings::ScrollBehavior scroll_behavior)
{
    // 1. Abort any ongoing smooth scroll for box.
    // 2. Resolve all pending scroll Promises whose scroll container is box.
    m_document->smooth_scroll_handler()->abort_any_ongoing_visual_viewport_scroll();

    HTML::TemporaryExecutionContext temporary_execution_context { m_document->realm() };
    // 3. Let scrollPromise be a new Promise.
    auto scroll_promise = WebIDL::create_promise(m_document->realm());

    // NB: Step 4 happens out of order, as we can start a smooth scroll or perform an instant scroll first.

    // 5. If the user agent honors the scroll-behavior property and one of the following is true:
    //      - behavior is "auto" and element is not null and its computed value of the scroll-behavior property is smooth, or
    //      - behavior is smooth
    //    then perform a smooth scroll of box to position; otherwise, perform an instant scroll of box to position.
    // NB: Above determination happens in SmoothScrollHandler::resolve_scroll_behavior.
    scroll_behavior = SmoothScrollHandler::resolve_scroll_behavior(*this, scroll_behavior);
    if (scroll_behavior == Bindings::ScrollBehavior::Smooth) {
        // NB: In this branch steps 6 and 7 are handled by SmoothScrollHander.
        m_document->smooth_scroll_handler()->start_visual_viewport_scroll(scroll_promise, offset);
    } else {
        // 6. Wait until either the position has finished updating, or scrollPromise has been resolved.
        set_offset(offset);
        // 7. If scrollPromise is still in the pending state:
        //      FIXME: 1. If the scroll position changed as a result of this call, emit the scrollend event.
        //      2. Resolve scrollPromise.
        WebIDL::resolve_promise(m_document->realm(), scroll_promise);
    }

    // 4. Return scrollPromise, and run the remaining steps in parallel.
    return scroll_promise;
}

GC::Ref<WebIDL::Promise> VisualViewport::perform_scroll_of_visual_viewport_scrolling_box_by_delta(CSSPixelPoint delta, Bindings::ScrollBehavior scroll_behavior)
{
    CSSPixelPoint target_offset;
    auto ongoing_visual_viewport_smooth_scroll = m_document->smooth_scroll_handler()->ongoing_visual_viewport_scroll();
    if (ongoing_visual_viewport_smooth_scroll) {
        auto ongoing_scroll_target_offset = ongoing_visual_viewport_smooth_scroll->target_offset();

        target_offset = ongoing_scroll_target_offset + delta;
    } else {
        target_offset = m_offset + delta;
    }

    return perform_scroll_of_visual_viewport_scrolling_box(target_offset, scroll_behavior);
}

Gfx::AffineTransform VisualViewport::transform() const
{
    Gfx::AffineTransform transform;
    auto offset = m_offset.to_type<double>() * m_scale;
    transform.translate(-offset.x(), -offset.y());
    transform.scale({ m_scale, m_scale });
    return transform;
}

void VisualViewport::set_offset(CSSPixelPoint offset)
{
    auto did_change = offset != m_offset;
    m_offset = offset;

    if (did_change) {
        m_document->set_needs_accumulated_visual_contexts_update(true);
        m_document->set_needs_repaint(Badge<CSS::VisualViewport> {}, InvalidateDisplayList::Yes);
    }
}

void VisualViewport::scroll_by(CSSPixelPoint delta)
{
    set_offset(m_offset + delta);
}

void VisualViewport::zoom(CSSPixelPoint position, double scale_delta)
{
    m_document->smooth_scroll_handler()->abort_any_ongoing_visual_viewport_scroll();

    static constexpr double MIN_ALLOWED_SCALE = 1.0;
    static constexpr double MAX_ALLOWED_SCALE = 5.0;
    double new_scale = clamp(m_scale * (1 + scale_delta), MIN_ALLOWED_SCALE, MAX_ALLOWED_SCALE);
    double applied_delta = new_scale / m_scale;

    // For pinch zoom we want focal_point to stay put on screen:
    // scale_new * (focal_point - offset_new) = scale_old * (focal_point - offset_old)
    auto new_offset = m_offset.to_type<double>() * m_scale * applied_delta;
    new_offset += position.to_type<int>().to_type<double>() * (applied_delta - 1.0f);

    auto viewport_float_size = m_document->navigable()->viewport_rect().size().to_type<double>();
    auto max_x_offset = max(0.0, viewport_float_size.width() * (new_scale - 1.0f));
    auto max_y_offset = max(0.0, viewport_float_size.height() * (new_scale - 1.0f));
    new_offset = { clamp(new_offset.x(), 0.0f, max_x_offset), clamp(new_offset.y(), 0.0f, max_y_offset) };

    m_scale = new_scale;
    set_offset((new_offset / m_scale).to_type<CSSPixels>());
}

CSSPixelPoint VisualViewport::map_to_layout_viewport(CSSPixelPoint position) const
{
    auto inverse = transform().inverse().value_or({});
    return inverse.map(position.to_type<int>()).to_type<CSSPixels>();
}

void VisualViewport::reset()
{
    m_document->smooth_scroll_handler()->abort_any_ongoing_visual_viewport_scroll();

    m_scale = 1.0;
    set_offset({ 0, 0 });
}

}
