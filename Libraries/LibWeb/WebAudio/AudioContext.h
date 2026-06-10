/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/AudioContext.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/MediaElementAudioSourceNode.h>

namespace Web::WebAudio {

using AudioContextOptions = Bindings::AudioContextOptions;
using AudioTimestamp = Bindings::AudioTimestamp;

// https://webaudio.github.io/web-audio-api/#AudioContext
class AudioContext final : public BaseAudioContext {
    WEB_WRAPPABLE(AudioContext, BaseAudioContext);
    GC_DECLARE_ALLOCATOR(AudioContext);

public:
    static WebIDL::ExceptionOr<GC::Ref<AudioContext>> construct_impl(JS::Realm&, AudioContextOptions const&);
    static WebIDL::ExceptionOr<GC::Ref<AudioContext>> create_for_constructor(GC::Ref<DOM::EventTarget> relevant_global_object, HTML::EnvironmentSettingsObject&, Optional<AudioContextOptions> const& context_options = {});

    virtual ~AudioContext() override;

    double base_latency() const { return m_base_latency; }
    double output_latency() const { return m_output_latency; }
    AudioTimestamp get_output_timestamp();
    WebIDL::ExceptionOr<void> resume(JS::Realm&, GC::Ref<WebIDL::Promise>);
    WebIDL::ExceptionOr<void> suspend(JS::Realm&, GC::Ref<WebIDL::Promise>);
    WebIDL::ExceptionOr<void> close(JS::Realm&, GC::Ref<WebIDL::Promise>);

    WebIDL::ExceptionOr<GC::Ref<MediaElementAudioSourceNode>> create_media_element_source(GC::Ptr<HTML::HTMLMediaElement>);

private:
    explicit AudioContext(GC::Ref<DOM::EventTarget> relevant_global_object)
        : BaseAudioContext(relevant_global_object)
    {
    }
    virtual void visit_edges(Cell::Visitor&) override;

    double m_base_latency { 0 };
    double m_output_latency { 0 };

    bool m_allowed_to_start = true;
    Vector<GC::Ref<WebIDL::Promise>> m_pending_resume_promises;
    bool m_suspended_by_user = false;

    bool start_rendering_audio_graph();
};

}
