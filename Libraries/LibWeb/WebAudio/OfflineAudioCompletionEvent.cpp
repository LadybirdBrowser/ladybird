/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebAudio/OfflineAudioCompletionEvent.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(OfflineAudioCompletionEvent);

GC::Ref<OfflineAudioCompletionEvent> OfflineAudioCompletionEvent::create(FlyString const& event_name, OfflineAudioCompletionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<OfflineAudioCompletionEvent>(event_name, event_init, time_stamp);
}

OfflineAudioCompletionEvent::OfflineAudioCompletionEvent(FlyString const& event_name, OfflineAudioCompletionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_rendered_buffer(event_init.rendered_buffer)
{
}

OfflineAudioCompletionEvent::~OfflineAudioCompletionEvent() = default;

void OfflineAudioCompletionEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_rendered_buffer);
}

}
