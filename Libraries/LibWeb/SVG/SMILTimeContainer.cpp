/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Animations/DocumentTimeline.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/SVG/SMILAnimation.h>
#include <LibWeb/SVG/SMILTimeContainer.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SMILTimeContainer);

GC::Ref<SMILTimeContainer> SMILTimeContainer::create(DOM::Document& document)
{
    return GC::Heap::the().allocate<SMILTimeContainer>(document);
}

// https://svgwg.org/specs/animations/#SVGElementTimelinebeginAttribute
// The outermost svg element of an SVG document fragment defines a timeline. Absolute times used by animation elements
// in an SVG document fragment are relative to the document fragment's timeline.
// NB: The animations of the timeline play on the document timeline, with their current time kept equal to the
//     timeline's: while the timeline runs, they start at the time on the document timeline at which its current time
//     is zero, and while it is paused, they are held at the time it was paused at. Until the timeline begins, they
//     stay idle.
SMILTimeContainer::SMILTimeContainer(DOM::Document& document)
    : m_document(document)
{
}

void SMILTimeContainer::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    visitor.visit(m_animations);
}

double SMILTimeContainer::document_timeline_time() const
{
    auto time = m_document->timeline()->current_time();
    return time.has_value() ? time->value : 0.;
}

// https://svgwg.org/specs/animations/#SVGElementTimelinebeginAttribute
void SMILTimeContainer::begin()
{
    // 'loadend'
    //     The document fragment's timeline starts the moment the load event for the outermost svg element is
    //     triggered.
    // NB: The caller decides when that moment is.
    VERIFY(!m_has_begun);
    m_has_begun = true;

    // NB: Seek to the time that setCurrentTime() recorded before the timeline began, if any.
    auto time = m_time_to_seek_to_once_begun.value_or(0.);
    m_time_to_seek_to_once_begun.clear();
    if (m_is_paused)
        m_paused_time = time;
    else
        m_zero_time = document_timeline_time() - time;
    synchronize_animations();
}

// https://svgwg.org/specs/animations/#__svg__SVGSVGElement__pauseAnimations
void SMILTimeContainer::pause()
{
    // The pauseAnimations and unpauseAnimations methods are used to pause and unpause all of the animations within
    // the SVG document fragment.
    if (m_is_paused)
        return;
    if (m_has_begun)
        m_paused_time = current_time_in_milliseconds();
    m_is_paused = true;
    synchronize_animations();
}

// https://svgwg.org/specs/animations/#__svg__SVGSVGElement__unpauseAnimations
void SMILTimeContainer::unpause()
{
    // The pauseAnimations and unpauseAnimations methods are used to pause and unpause all of the animations within
    // the SVG document fragment.
    if (!m_is_paused)
        return;
    m_is_paused = false;
    if (m_has_begun)
        m_zero_time = document_timeline_time() - m_paused_time;
    synchronize_animations();
}

// https://svgwg.org/specs/animations/#__svg__SVGSVGElement__getCurrentTime
double SMILTimeContainer::current_time_in_milliseconds() const
{
    // Return the current time in seconds relative to the start time of the timeline.
    // NB: This returns it in milliseconds, the unit of the document timeline.
    VERIFY(m_has_begun);
    if (m_is_paused)
        return m_paused_time;
    return document_timeline_time() - m_zero_time;
}

// https://svgwg.org/specs/animations/#__svg__SVGSVGElement__setCurrentTime
void SMILTimeContainer::seek_to_time_in_milliseconds(double time)
{
    // If the timeline for the current SVG document fragment has begun, then the timeline's current time (in seconds,
    // relative to the start time of the timeline) is set to seconds.
    VERIFY(m_has_begun);

    // NB: Like other engines, clamp a negative time to the start of the timeline, as SVG Tiny 1.2 does.
    time = max(time, 0.);
    if (m_is_paused)
        m_paused_time = time;
    else
        m_zero_time = document_timeline_time() - time;
    synchronize_animations();
}

// https://svgwg.org/specs/animations/#__svg__SVGSVGElement__setCurrentTime
void SMILTimeContainer::record_time_to_seek_to_once_begun_in_milliseconds(double time)
{
    // Otherwise, the timeline has not begun. seconds is recorded on the current SVG document fragment as the time to be
    // seeked to once the timeline has begun.
    VERIFY(!m_has_begun);
    m_time_to_seek_to_once_begun = max(time, 0.);
}

void SMILTimeContainer::add_animation(SMILAnimation& animation)
{
    m_animations.append(animation);
    synchronize_animation(animation);
}

void SMILTimeContainer::remove_animation(SMILAnimation& animation)
{
    m_animations.remove_first_matching([&](auto const& entry) { return entry.ptr() == &animation; });
}

void SMILTimeContainer::synchronize_animations()
{
    for (auto& animation : m_animations)
        synchronize_animation(animation);
}

// https://drafts.csswg.org/web-animations-1/#setting-the-start-time-of-an-animation
// https://drafts.csswg.org/web-animations-1/#setting-the-current-time-of-an-animation
void SMILTimeContainer::synchronize_animation(SMILAnimation& animation)
{
    // NB: Both procedures take effect synchronously, and update the finished state of the animation as a seek does. A
    //     backward seek therefore applies to an animation that already finished, too.
    if (!m_has_begun)
        return;

    HTML::TemporaryExecutionContext execution_context { m_document->relevant_settings_object() };
    if (m_is_paused) {
        // NB: Making the start time unresolved holds the animation at its current time, which setting the current time
        //     then moves to the time the timeline is paused at.
        MUST(animation.set_start_time_for_bindings(Animations::NullableCSSNumberish { Empty {} }));
        MUST(animation.set_current_time_for_bindings(Animations::NullableCSSNumberish { m_paused_time }));
        return;
    }
    MUST(animation.set_start_time_for_bindings(Animations::NullableCSSNumberish { m_zero_time }));
}

}
