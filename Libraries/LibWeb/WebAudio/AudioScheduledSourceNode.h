/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/WebAudio/AudioNode.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#AudioScheduledSourceNode
class AudioScheduledSourceNode : public AudioNode {
    WEB_PLATFORM_OBJECT(AudioScheduledSourceNode, AudioNode);
    GC_DECLARE_ALLOCATOR(AudioScheduledSourceNode);

public:
    virtual ~AudioScheduledSourceNode() override;

    GC::Ptr<WebIDL::CallbackType> onended();
    void set_onended(GC::Ptr<WebIDL::CallbackType>);

    WebIDL::ExceptionOr<void> start(f64 when = 0);
    WebIDL::ExceptionOr<void> stop(f64 when = 0);

    // https://webaudio.github.io/web-audio-api/#dom-audioscheduledsourcenode-source-started-slot
    // Exposed as an internal helper for the rendering implementation.
    bool source_started_for_rendering() const { return m_source_started; }

    // Exposed as internal helpers for the rendering implementation.
    // These are the scheduled times captured from start()/stop() calls.
    Optional<f64> start_when_for_rendering() const { return m_start_when; }
    Optional<f64> stop_when_for_rendering() const { return m_stop_when; }

protected:
    AudioScheduledSourceNode(JS::Realm&, GC::Ref<BaseAudioContext>);

    bool source_started() const { return m_source_started; }
    void set_source_started(bool started) { m_source_started = started; }

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    // Control-thread owned scheduling state.
    Optional<f64> m_start_when;
    Optional<f64> m_stop_when;

private:
    // https://webaudio.github.io/web-audio-api/#dom-audioscheduledsourcenode-source-started-slot
    bool m_source_started { false };
};

}
