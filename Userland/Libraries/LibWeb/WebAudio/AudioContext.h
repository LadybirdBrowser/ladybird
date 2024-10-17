/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/AudioContextPrototype.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>

namespace Web::WebAudio {

struct AudioContextOptions {
    Bindings::AudioContextLatencyCategory latency_hint = Bindings::AudioContextLatencyCategory::Interactive;
    Optional<float> sample_rate;
};

struct AudioTimestamp {
    double context_time { 0 };
    double performance_time { 0 };
};

// https://webaudio.github.io/web-audio-api/#AudioContext
class AudioContext final : public BaseAudioContext {
    WEB_PLATFORM_OBJECT(AudioContext, BaseAudioContext);
    JS_DECLARE_ALLOCATOR(AudioContext);

public:
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioContext>> construct_impl(JS::Realm&, AudioContextOptions const& context_options = {});

    virtual ~AudioContext() override;

    double base_latency() const { return m_base_latency; }
    double output_latency() const { return m_output_latency; }
    AudioTimestamp get_output_timestamp();
    WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::Promise>> resume();
    WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::Promise>> suspend();
    WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::Promise>> close();

private:
    explicit AudioContext(JS::Realm&, AudioContextOptions const& context_options);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    double m_base_latency { 0 };
    double m_output_latency { 0 };

    bool m_allowed_to_start = true;
    Vector<JS::NonnullGCPtr<WebIDL::Promise>> m_pending_resume_promises;
    bool m_suspended_by_user = false;

    bool start_rendering_audio_graph();
};

}
