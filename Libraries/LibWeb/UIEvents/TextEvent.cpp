/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/UIEvents/TextEvent.h>

namespace Web::UIEvents {

GC_DEFINE_ALLOCATOR(TextEvent);

GC::Ref<TextEvent> TextEvent::create(FlyString const& event_name, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<TextEvent>(event_name, time_stamp);
}

TextEvent::TextEvent(FlyString const& event_name, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : UIEvent(event_name, time_stamp)
{
}

TextEvent::~TextEvent() = default;

// https://w3c.github.io/uievents/#dom-textevent-inittextevent
void TextEvent::init_text_event(String const& type, bool bubbles, bool cancelable, GC::Ptr<HTML::WindowProxy> view, String const& data)
{
    // Initializes attributes of a TextEvent object. This method has the same behavior as UIEvent.initUIEvent().
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
