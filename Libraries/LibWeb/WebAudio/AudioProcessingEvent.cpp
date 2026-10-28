/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebAudio/AudioProcessingEvent.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(AudioProcessingEvent);

GC::Ref<AudioProcessingEvent> AudioProcessingEvent::create(Utf16FlyString const& event_name, AudioProcessingEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
{
    return GC::Heap::the().allocate<AudioProcessingEvent>(event_name, event_init, time_stamp);
}

AudioProcessingEvent::AudioProcessingEvent(Utf16FlyString const& event_name, AudioProcessingEventInit const& event_init, HighResolutionTime::DOMHighResTimeStamp time_stamp)
    : DOM::Event(event_name, event_init, time_stamp)
    , m_playback_time(event_init.playback_time)
    , m_input_buffer(event_init.input_buffer)
    , m_output_buffer(event_init.output_buffer)
{
}

AudioProcessingEvent::~AudioProcessingEvent() = default;

void AudioProcessingEvent::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_input_buffer);
    visitor.visit(m_output_buffer);
}

}
