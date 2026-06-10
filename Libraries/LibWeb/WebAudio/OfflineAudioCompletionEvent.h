/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/OfflineAudioCompletionEvent.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>
#include <LibWeb/WebAudio/AudioBuffer.h>

namespace Web::HTML {

class Window;

}

namespace Web::WebAudio {

using OfflineAudioCompletionEventInit = Bindings::OfflineAudioCompletionEventInit;

class OfflineAudioCompletionEvent final : public DOM::Event {
    WEB_WRAPPABLE(OfflineAudioCompletionEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(OfflineAudioCompletionEvent);

public:
    static GC::Ref<OfflineAudioCompletionEvent> create(FlyString const& event_name, OfflineAudioCompletionEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual ~OfflineAudioCompletionEvent() override;

    GC::Ptr<AudioBuffer> rendered_buffer() const { return m_rendered_buffer; }

private:
    OfflineAudioCompletionEvent(FlyString const& event_name, OfflineAudioCompletionEventInit const&, HighResolutionTime::DOMHighResTimeStamp);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ptr<AudioBuffer> m_rendered_buffer;
};

}
