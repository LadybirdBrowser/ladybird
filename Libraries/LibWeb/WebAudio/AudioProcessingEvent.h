/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Event.h>
#include <LibWeb/WebAudio/AudioBuffer.h>

namespace Web::WebAudio {

struct AudioProcessingEventInit : public DOM::EventInit {
    double playback_time { 0.0 };
    GC::Ptr<AudioBuffer> input_buffer;
    GC::Ptr<AudioBuffer> output_buffer;
};

class AudioProcessingEvent final : public DOM::Event {
    WEB_PLATFORM_OBJECT(AudioProcessingEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(AudioProcessingEvent);

public:
    static WebIDL::ExceptionOr<GC::Ref<AudioProcessingEvent>> construct_impl(JS::Realm&, FlyString const& event_name, AudioProcessingEventInit const& event_init);

    virtual ~AudioProcessingEvent() override;

    double playback_time() const { return m_playback_time; }
    GC::Ptr<AudioBuffer> input_buffer() const { return m_input_buffer; }
    GC::Ptr<AudioBuffer> output_buffer() const { return m_output_buffer; }

private:
    AudioProcessingEvent(JS::Realm&, FlyString const& event_name, AudioProcessingEventInit const& event_init);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    double m_playback_time { 0.0 };
    GC::Ptr<AudioBuffer> m_input_buffer;
    GC::Ptr<AudioBuffer> m_output_buffer;
};

}
