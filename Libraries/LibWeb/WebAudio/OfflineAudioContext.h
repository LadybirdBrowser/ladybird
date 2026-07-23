/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Ben Eidson <b.e.eidson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/RefPtr.h>
#include <LibWeb/Bindings/OfflineAudioContext.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/Rendering/OfflineAudioRenderer.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#OfflineAudioContext
class OfflineAudioContext final : public BaseAudioContext {
    WEB_PLATFORM_OBJECT(OfflineAudioContext, BaseAudioContext);
    GC_DECLARE_ALLOCATOR(OfflineAudioContext);

public:
    static WebIDL::ExceptionOr<GC::Ref<OfflineAudioContext>> construct_impl(JS::Realm&, Bindings::OfflineAudioContextOptions const&);
    static WebIDL::ExceptionOr<GC::Ref<OfflineAudioContext>> construct_impl(
        JS::Realm&,
        WebIDL::UnsignedLong number_of_channels,
        WebIDL::UnsignedLong length,
        float sample_rate);

    virtual ~OfflineAudioContext() override;

    WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> start_rendering();
    WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> resume();
    WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> suspend(double suspend_time);

    WebIDL::UnsignedLong length() const;

    GC::Ptr<WebIDL::CallbackType> oncomplete();
    void set_oncomplete(GC::Ptr<WebIDL::CallbackType>);

private:
    OfflineAudioContext(JS::Realm&, WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void document_became_inactive() override;

    WebIDL::UnsignedLong m_length {};
    WebIDL::UnsignedLong m_number_of_channels {};
    bool m_rendering_started { false };

    GC::Ptr<AudioBuffer> m_rendered_buffer;

    RefPtr<Rendering::OfflineAudioRenderer> m_renderer;

    // https://webaudio.github.io/web-audio-api/#dom-offlineaudiocontext-suspend, keyed by suspend frame.
    HashMap<u64, GC::Ref<WebIDL::Promise>> m_suspend_promises;

    void begin_offline_rendering(GC::Ref<WebIDL::Promise> promise);
    void finish_rendering(GC::Ref<WebIDL::Promise> promise);
    void handle_suspended(double suspend_time);
    void queue_a_statechange_event();
};

}
