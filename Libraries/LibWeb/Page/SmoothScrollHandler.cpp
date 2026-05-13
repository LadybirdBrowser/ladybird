/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Badge.h>
#include <AK/StdLibExtras.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Ptr.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/Window.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/PseudoElement.h>
#include <LibWeb/CSS/VisualViewport.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Page/SmoothScrollHandler.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web {

GC_DEFINE_ALLOCATOR(SmoothScrollTask);
GC_DEFINE_ALLOCATOR(SmoothScrollHandler);

SmoothScrollTask::SmoothScrollTask(GC::Ref<WebIDL::Promise> promise, CSSPixelPoint source_offset, CSSPixelPoint target_offset, double timestamp)
    : m_scroll_promise(promise)
    , m_source_offset(source_offset)
    , m_target_offset(target_offset)
    , m_start_time(timestamp)
{
    m_is_ongoing = true;

    update_duration();
}

void SmoothScrollTask::update_duration()
{
    static constexpr double SCROLL_SPEED_IN_PIXELS_PER_SECOND = 10000;
    static constexpr double SCROLL_MIN_DURATION = 250;

    auto delta = target_offset() - source_offset();
    auto distance = abs(delta.x().to_double()) + abs(delta.y().to_double());

    m_duration = max((distance / (SCROLL_SPEED_IN_PIXELS_PER_SECOND)) * 1000, SCROLL_MIN_DURATION);
}

void SmoothScrollTask::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_scroll_promise);
}

GC::Ref<SmoothScrollHandler> SmoothScrollHandler::create(GC::Heap& heap, GC::Ref<DOM::Document> document)
{
    return heap.allocate<SmoothScrollHandler>(document);
}

SmoothScrollHandler::SmoothScrollHandler(GC::Ref<DOM::Document> document)
    : m_document(document)
{
}

void SmoothScrollHandler::process()
{
    auto timestamp = HighResolutionTime::unsafe_shared_current_time();

    if (m_ongoing_viewport_scroll) {
        if (m_ongoing_viewport_scroll->is_ongoing()) {
            auto progress = task_progress(*m_ongoing_viewport_scroll, timestamp);
            CSSPixelPoint current_offset = current_scroll_offset(*m_ongoing_viewport_scroll, progress);

            m_document->navigable()->set_viewport_scroll_offset(current_offset);

            if (progress >= 1) {
                finish(*m_ongoing_viewport_scroll);
                m_ongoing_viewport_scroll = nullptr;
            }
        } else {
            m_ongoing_viewport_scroll = nullptr;
        }
    }

    if (m_ongoing_visual_viewport_scroll) {
        if (m_ongoing_visual_viewport_scroll->is_ongoing()) {
            auto progress = task_progress(*m_ongoing_visual_viewport_scroll, timestamp);
            CSSPixelPoint current_offset = current_scroll_offset(*m_ongoing_visual_viewport_scroll, progress);

            m_document->visual_viewport()->set_offset(current_offset);

            if (progress >= 1) {
                finish(*m_ongoing_visual_viewport_scroll);
                m_ongoing_visual_viewport_scroll = nullptr;
            }
        } else {
            m_ongoing_visual_viewport_scroll = nullptr;
        }
    }
}

void SmoothScrollHandler::start_viewport_scroll(GC::Ref<WebIDL::Promise> promise, CSSPixelPoint target_offset)
{
    VERIFY(!m_ongoing_viewport_scroll);

    auto source_offset = m_document->navigable()->viewport_scroll_offset();
    m_ongoing_viewport_scroll = heap().allocate<SmoothScrollTask>(promise, source_offset, target_offset, HighResolutionTime::unsafe_shared_current_time());
}

void SmoothScrollHandler::start_visual_viewport_scroll(GC::Ref<WebIDL::Promise> promise, CSSPixelPoint target_offset)
{
    VERIFY(!m_ongoing_visual_viewport_scroll);

    auto source_offset = m_document->visual_viewport()->offset();
    m_ongoing_visual_viewport_scroll = heap().allocate<SmoothScrollTask>(promise, source_offset, target_offset, HighResolutionTime::unsafe_shared_current_time());
}

void SmoothScrollHandler::abort_any_ongoing_viewport_scroll()
{
    if (!m_ongoing_viewport_scroll)
        return;

    finish(*m_ongoing_viewport_scroll);
    m_ongoing_viewport_scroll = nullptr;
}

void SmoothScrollHandler::abort_any_ongoing_visual_viewport_scroll()
{
    if (!m_ongoing_visual_viewport_scroll)
        return;

    finish(*m_ongoing_visual_viewport_scroll);
    m_ongoing_visual_viewport_scroll = nullptr;
}

// https://drafts.csswg.org/cssom-view-1/#ref-for-propdef-scroll-behavior
Bindings::ScrollBehavior SmoothScrollHandler::resolve_scroll_behavior(DOM::Element const& element, Bindings::ScrollBehavior scroll_behavior)
{
    // If the user agent honors the scroll-behavior property and one of the following is true:

    // - behavior is "auto" and element is not null and its computed value of the scroll-behavior property is smooth,
    //   or
    // - behavior is smooth
    if ((scroll_behavior == Bindings::ScrollBehavior::Auto
            && element.computed_properties()
            && element.computed_properties()->scroll_behavior() == CSS::ScrollBehavior::Smooth)
        || scroll_behavior == Bindings::ScrollBehavior::Smooth) {
        // then perform a smooth scroll of box to position;
        return Bindings::ScrollBehavior::Smooth;
    }

    // otherwise, perform an instant scroll of box to position.
    return Bindings::ScrollBehavior::Instant;
}

Bindings::ScrollBehavior SmoothScrollHandler::resolve_scroll_behavior(HTML::Navigable const& navigable, Bindings::ScrollBehavior scroll_behavior)
{
    if (auto doc = navigable.active_document()) {
        if (auto element = doc->scrolling_element()) {
            return resolve_scroll_behavior(*element, scroll_behavior);
        }
    }

    return Bindings::ScrollBehavior::Instant;
}

Bindings::ScrollBehavior SmoothScrollHandler::resolve_scroll_behavior(CSS::VisualViewport& visual_viewport, Bindings::ScrollBehavior scroll_behavior)
{
    if (auto doc = visual_viewport.document()) {
        if (auto element = doc->scrolling_element()) {
            return resolve_scroll_behavior(*element, scroll_behavior);
        }
    }

    return Bindings::ScrollBehavior::Instant;
}

void SmoothScrollHandler::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);

    visitor.visit(m_ongoing_viewport_scroll);
    visitor.visit(m_ongoing_visual_viewport_scroll);
}

// Steps 6 and 7 of https://drafts.csswg.org/cssom-view-1/#perform-a-scroll
void SmoothScrollHandler::finish(SmoothScrollTask& task)
{
    task.set_is_ongoing(false);
    // 6. Wait until either the position has finished updating, or scrollPromise has been resolved.

    HTML::TemporaryExecutionContext execution_context { m_document->realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
    // 7. If scrollPromise is still in the pending state:
    //      FIXME: 1. If the scroll position changed as a result of this call, emit the scrollend event.
    //      2. Resolve scrollPromise.
    WebIDL::resolve_promise(m_document->realm(), task.scroll_promise());
}

CSSPixelPoint SmoothScrollHandler::current_scroll_offset(SmoothScrollTask& task, double progress)
{
    CSSPixelPoint source_offset = task.source_offset();
    CSSPixelPoint target_offset = task.target_offset();

    auto t = ease(progress);

    auto x = mix(source_offset.x().to_double(), target_offset.x().to_double(), t);
    auto y = mix(source_offset.y().to_double(), target_offset.y().to_double(), t);

    return { x, y };
}

double SmoothScrollHandler::task_progress(SmoothScrollTask& task, double timestamp)
{
    return clamp((timestamp - task.start_time()) / task.duration(), 0, 1);
}

double SmoothScrollHandler::ease(double t)
{
    return 1 - ((1 - t) * (1 - t));
}

}
