/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/AudioProcessingEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/WebAudio/AudioBuffer.h>
#include <LibWebCommon/HighResolutionTime/DOMHighResTimeStamp.h>

namespace Web::WebAudio {

using AudioProcessingEventInit = Bindings::AudioProcessingEventInit;

// https://webaudio.github.io/web-audio-api/#AudioProcessingEvent
class AudioProcessingEvent final : public DOM::Event {
    WEB_WRAPPABLE(AudioProcessingEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(AudioProcessingEvent);

public:
    static GC::Ref<AudioProcessingEvent> create(Utf16FlyString const& event_name, AudioProcessingEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual ~AudioProcessingEvent() override;

    double playback_time() const { return m_playback_time; }
    GC::Ref<AudioBuffer> input_buffer() const { return m_input_buffer; }
    GC::Ref<AudioBuffer> output_buffer() const { return m_output_buffer; }

private:
    AudioProcessingEvent(Utf16FlyString const& event_name, AudioProcessingEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    double m_playback_time { 0 };
    GC::Ref<AudioBuffer> m_input_buffer;
    GC::Ref<AudioBuffer> m_output_buffer;
};

}
