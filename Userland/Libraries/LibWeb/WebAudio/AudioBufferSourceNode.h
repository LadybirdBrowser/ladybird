/*
 * Copyright (c) 2024, Bar Yemini <bar.ye651@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/AudioBufferSourceNodePrototype.h>
#include <LibWeb/WebAudio/AudioScheduledSourceNode.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#AudioBufferSourceOptions
struct AudioBufferSourceOptions {
    JS::GCPtr<AudioBuffer> buffer;
    float detune { 0 };
    bool loop { false };
    double loop_end { 0 };
    double loop_start { 0 };
    float playback_rate { 1 };
};

// https://webaudio.github.io/web-audio-api/#AudioBufferSourceNode
class AudioBufferSourceNode : public AudioScheduledSourceNode {
    WEB_PLATFORM_OBJECT(AudioBufferSourceNode, AudioScheduledSourceNode);
    JS_DECLARE_ALLOCATOR(AudioBufferSourceNode);

public:
    virtual ~AudioBufferSourceNode() override;

    static WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioBufferSourceNode>> create(JS::Realm&, JS::NonnullGCPtr<BaseAudioContext>, AudioBufferSourceOptions const& = {});
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioBufferSourceNode>> construct_impl(JS::Realm&, JS::NonnullGCPtr<BaseAudioContext>, AudioBufferSourceOptions const& = {});

protected:
    AudioBufferSourceNode(JS::Realm&, JS::NonnullGCPtr<BaseAudioContext>, AudioBufferSourceOptions const& = {});

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
};

}
