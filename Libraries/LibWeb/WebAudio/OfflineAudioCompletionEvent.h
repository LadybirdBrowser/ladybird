/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Event.h>
#include <LibWeb/WebAudio/AudioBuffer.h>

namespace Web::WebAudio {

struct OfflineAudioCompletionEventInit : public DOM::EventInit {
    GC::Ptr<AudioBuffer> rendered_buffer;
};

class OfflineAudioCompletionEvent final : public DOM::Event {
    WEB_PLATFORM_OBJECT(OfflineAudioCompletionEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(OfflineAudioCompletionEvent);

public:
    static WebIDL::ExceptionOr<GC::Ref<OfflineAudioCompletionEvent>> construct_impl(JS::Realm&, FlyString const& event_name, OfflineAudioCompletionEventInit const& event_init);

    virtual ~OfflineAudioCompletionEvent() override;

    GC::Ptr<AudioBuffer> rendered_buffer() const { return m_rendered_buffer; }

private:
    OfflineAudioCompletionEvent(JS::Realm&, FlyString const& event_name, OfflineAudioCompletionEventInit const& event_init);

    void initialize(JS::Realm&) override;

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<AudioBuffer> m_rendered_buffer;
};

}
