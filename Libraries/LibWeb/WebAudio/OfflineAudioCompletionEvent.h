/*
 * Copyright (c) 2025, Ben Eidson <b.e.eidson@gmail.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Event.h>
#include <LibWeb/WebAudio/AudioBuffer.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#OfflineAudioCompletionEventInit
struct OfflineAudioCompletionEventInit : public DOM::EventInit {
    GC::Ptr<AudioBuffer> rendered_buffer;
};

// https://webaudio.github.io/web-audio-api/#OfflineAudioCompletionEvent
class OfflineAudioCompletionEvent : public DOM::Event {
    WEB_PLATFORM_OBJECT(OfflineAudioCompletionEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(OfflineAudioCompletionEvent);

public:
    [[nodiscard]] static GC::Ref<OfflineAudioCompletionEvent> create(JS::Realm&, FlyString const& type, OfflineAudioCompletionEventInit const& event_init);
    static WebIDL::ExceptionOr<GC::Ref<OfflineAudioCompletionEvent>> construct_impl(JS::Realm&, FlyString const& type, OfflineAudioCompletionEventInit const& event_init);

    virtual ~OfflineAudioCompletionEvent() override;

    // https://webaudio.github.io/web-audio-api/#dom-offlineaudiocompletionevent-renderedbuffer
    GC::Ref<AudioBuffer> rendered_buffer() const { return m_rendered_buffer; }

private:
    OfflineAudioCompletionEvent(JS::Realm&, FlyString const& type, OfflineAudioCompletionEventInit const& event_init);

    void initialize(JS::Realm&) override;

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<AudioBuffer> m_rendered_buffer;
};

}
