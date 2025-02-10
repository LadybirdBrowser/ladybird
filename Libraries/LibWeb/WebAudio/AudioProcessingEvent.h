/*
 * Copyright (c) 2025, Bar Yemini <bar.ye651@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/WebAudio/AudioBuffer.h>

namespace Web::WebAudio {

struct AudioProcessingEventInit : public DOM::EventInit {
    double playback_time;
    GC::Ptr<AudioBuffer> input_buffer;
    GC::Ptr<AudioBuffer> output_buffer;
};

class AudioProcessingEvent final : public DOM::Event {
    WEB_PLATFORM_OBJECT(AudioProcessingEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(AudioProcessingEvent);

public:
    [[nodiscard]] static GC::Ref<AudioProcessingEvent> create(JS::Realm&, FlyString const& event_name, AudioProcessingEventInit const&);
    [[nodiscard]] static GC::Ref<AudioProcessingEvent> construct_impl(JS::Realm&, FlyString const& event_name, AudioProcessingEventInit const&);

    double const& playback_time() const { return m_playback_time; }
    GC::Ptr<AudioBuffer> const& input_buffer() const { return m_input_buffer; }
    GC::Ptr<AudioBuffer> const& output_buffer() const { return m_output_buffer; }

private:
    AudioProcessingEvent(JS::Realm&, FlyString const& event_name, AudioProcessingEventInit const& event_init);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor& visitor) override;

    double m_playback_time;
    GC::Ptr<AudioBuffer> m_input_buffer;
    GC::Ptr<AudioBuffer> m_output_buffer;
};

}
