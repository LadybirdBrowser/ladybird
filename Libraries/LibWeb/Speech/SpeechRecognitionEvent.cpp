/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Bindings/SpeechRecognitionEvent.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Speech/SpeechRecognitionEvent.h>

namespace Web::Speech {

GC_DEFINE_ALLOCATOR(SpeechRecognitionEvent);

WebIDL::ExceptionOr<GC::Ref<SpeechRecognitionEvent>> SpeechRecognitionEvent::construct_impl(HTML::Window& window, FlyString const& event_name, Bindings::SpeechRecognitionEventInit const& event_init)
{
    return GC::Heap::the().allocate<SpeechRecognitionEvent>(event_name, event_init, HighResolutionTime::current_high_resolution_time(HTML::relevant_global_object(window)));
}

SpeechRecognitionEvent::SpeechRecognitionEvent(FlyString const& event_name, Bindings::SpeechRecognitionEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
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
