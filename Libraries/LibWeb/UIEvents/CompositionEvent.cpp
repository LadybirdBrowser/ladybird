/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/CompositionEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/UIEvents/CompositionEvent.h>

namespace Web::UIEvents {

GC_DEFINE_ALLOCATOR(CompositionEvent);

static HighResolutionTime::DOMHighResTimeStamp event_time_stamp(HTML::Window& window)
{
    return HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(window));
}

GC::Ref<CompositionEvent> CompositionEvent::create(FlyString const& event_name, Bindings::CompositionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<CompositionEvent>(event_name, event_init, time_stamp);
}

WebIDL::ExceptionOr<GC::Ref<CompositionEvent>> CompositionEvent::construct_impl(HTML::Window& window, FlyString const& event_name, Bindings::CompositionEventInit const& event_init)
{
    return GC::Heap::the().allocate<CompositionEvent>(event_name, event_init, event_time_stamp(window));
}

CompositionEvent::CompositionEvent(FlyString const& event_name, Bindings::CompositionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : UIEvent(event_name, event_init, time_stamp)
    , m_data(event_init.data)
{
}

CompositionEvent::~CompositionEvent() = default;

// https://w3c.github.io/uievents/#dom-compositionevent-initcompositionevent
void CompositionEvent::init_composition_event(String const& type, bool bubbles, bool cancelable, GC::Ptr<HTML::WindowProxy> view, String const& data)
{
    // Initializes attributes of a CompositionEvent object. This method has the same behavior as UIEvent.initUIEvent().
    // The value of detail remains undefined.

    // 1. If this’s dispatch flag is set, then return.
    if (dispatched())
        return;

    // 2. Initialize this with type, bubbles, and cancelable.
    initialize_event(type, bubbles, cancelable);

    // Implementation Defined: Initialise other values.
    m_view = view;
    m_data = data;
}

}
