/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/InputEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/UIEvents/InputEvent.h>

namespace Web::UIEvents {

GC_DEFINE_ALLOCATOR(InputEvent);

static HighResolutionTime::DOMHighResTimeStamp event_time_stamp(HTML::Window& window)
{
    return HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(window));
}

GC::Ref<InputEvent> InputEvent::create_from_platform_event(FlyString const& event_name, Bindings::InputEventInit const& event_init, Vector<GC::Ref<DOM::StaticRange>> const& target_ranges, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    auto event = GC::Heap::the().allocate<InputEvent>(event_name, event_init, target_ranges, time_stamp);
    event->set_bubbles(true);
    if (event_name == "beforeinput"_fly_string) {
        event->set_cancelable(true);
    }
    return event;
}

WebIDL::ExceptionOr<GC::Ref<InputEvent>> InputEvent::construct_impl(HTML::Window& window, FlyString const& event_name, Bindings::InputEventInit const& event_init)
{
    return GC::Heap::the().allocate<InputEvent>(event_name, event_init, Vector<GC::Ref<DOM::StaticRange>> {}, event_time_stamp(window));
}

InputEvent::InputEvent(FlyString const& event_name, Bindings::InputEventInit const& event_init, Vector<GC::Ref<DOM::StaticRange>> const& target_ranges, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : UIEvent(event_name, event_init, time_stamp)
    , m_data(event_init.data)
    , m_is_composing(event_init.is_composing)
    , m_input_type(event_init.input_type)
    , m_target_ranges(target_ranges)
{
}

InputEvent::~InputEvent() = default;

void InputEvent::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_target_ranges);
}

// https://w3c.github.io/input-events/#dom-inputevent-gettargetranges
ReadonlySpan<GC::Ref<DOM::StaticRange>> InputEvent::get_target_ranges() const
{
    // getTargetRanges() returns an array of StaticRanges representing the content that the event will modify if it is
    // not canceled. The returned StaticRanges MUST cover only the code points that the browser would normally replace,
    // even if they are only part of a grapheme cluster.
    return m_target_ranges;
}

}
