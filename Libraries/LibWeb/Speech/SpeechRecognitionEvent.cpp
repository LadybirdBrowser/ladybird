/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Speech/SpeechRecognitionEvent.h>

namespace Web::Speech {

GC_DEFINE_ALLOCATOR(SpeechRecognitionEvent);

GC::Ref<SpeechRecognitionEvent> SpeechRecognitionEvent::create(FlyString const& event_name, SpeechRecognitionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<SpeechRecognitionEvent>(event_name, event_init, time_stamp);
}

GC::Ref<SpeechRecognitionEvent> SpeechRecognitionEvent::create(Utf16String const& event_name, SpeechRecognitionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<SpeechRecognitionEvent>(Utf16FlyString::from_utf16(event_name.utf16_view()), event_init, time_stamp);
}

GC::Ref<SpeechRecognitionEvent> SpeechRecognitionEvent::create(Utf16FlyString const& event_name, SpeechRecognitionEventInit const& event_init)
{
    return GC::Heap::the().allocate<SpeechRecognitionEvent>(event_name, event_init, 0);
}

SpeechRecognitionEvent::SpeechRecognitionEvent(FlyString const& event_name, SpeechRecognitionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_result_index(event_init.result_index)
    , m_results(event_init.results)
{
}

SpeechRecognitionEvent::SpeechRecognitionEvent(Utf16FlyString const& event_name, SpeechRecognitionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_result_index(event_init.result_index)
    , m_results(event_init.results)
{
}

void SpeechRecognitionEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_results);
}

}
