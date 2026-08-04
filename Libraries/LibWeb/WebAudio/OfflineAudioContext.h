/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Ben Eidson <b.e.eidson@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/OfflineAudioContext.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/Rendering/OfflineAudioRenderer.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebAudio {

using OfflineAudioContextOptions = Bindings::OfflineAudioContextOptions;

// https://webaudio.github.io/web-audio-api/#OfflineAudioContext
class OfflineAudioContext final : public BaseAudioContext {
    WEB_WRAPPABLE(OfflineAudioContext, BaseAudioContext);
    GC_DECLARE_ALLOCATOR(OfflineAudioContext);

public:
    static WebIDL::ExceptionOr<GC::Ref<OfflineAudioContext>> create_for_constructor(JS::Object&, OfflineAudioContextOptions const&);
    static WebIDL::ExceptionOr<GC::Ref<OfflineAudioContext>> create_for_constructor(JS::Object&, WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate);
    static WebIDL::ExceptionOr<GC::Ref<OfflineAudioContext>> create_for_constructor(GC::Ref<DOM::EventTarget> relevant_global_object, OfflineAudioContextOptions const&);
    static WebIDL::ExceptionOr<GC::Ref<OfflineAudioContext>> create_for_constructor(
        GC::Ref<DOM::EventTarget> relevant_global_object,
        WebIDL::UnsignedLong number_of_channels,
        WebIDL::UnsignedLong length,
        float sample_rate);

    virtual ~OfflineAudioContext() override;

    WebIDL::ExceptionOr<void> start_rendering(GC::Ref<WebIDL::Promise>);
    WebIDL::ExceptionOr<void> resume();
    WebIDL::ExceptionOr<void> resume(GC::Ref<WebIDL::Promise>);
    WebIDL::ExceptionOr<void> suspend(double suspend_time);
    WebIDL::ExceptionOr<void> suspend(double suspend_time, GC::Ref<WebIDL::Promise>);

    WebIDL::UnsignedLong length() const;

    GC::Ptr<WebIDL::CallbackType> oncomplete();
    void set_oncomplete(GC::Ptr<WebIDL::CallbackType>);

private:
    OfflineAudioContext(GC::Ref<DOM::EventTarget> relevant_global_object, WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate);
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
