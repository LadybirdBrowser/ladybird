/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/AudioProcessingEventPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioProcessingEvent.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(AudioProcessingEvent);

WebIDL::ExceptionOr<GC::Ref<AudioProcessingEvent>> AudioProcessingEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, AudioProcessingEventInit const& event_init)
{
    return realm.create<AudioProcessingEvent>(realm, event_name, event_init);
}

AudioProcessingEvent::AudioProcessingEvent(JS::Realm& realm, FlyString const& event_name, AudioProcessingEventInit const& event_init)
    : DOM::Event(realm, event_name, event_init)
    , m_playback_time(event_init.playback_time)
    , m_input_buffer(event_init.input_buffer)
    , m_output_buffer(event_init.output_buffer)
{
}

AudioProcessingEvent::~AudioProcessingEvent() = default;

void AudioProcessingEvent::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AudioProcessingEvent);
    Base::initialize(realm);
}

void AudioProcessingEvent::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_input_buffer);
    visitor.visit(m_output_buffer);
}

}
