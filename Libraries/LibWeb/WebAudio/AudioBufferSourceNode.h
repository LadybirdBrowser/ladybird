/*
 * Copyright (c) 2024, Bar Yemini <bar.ye651@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/AudioBufferSourceNodePrototype.h>
#include <LibWeb/WebAudio/AudioBuffer.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/AudioScheduledSourceNode.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#AudioBufferSourceOptions
struct AudioBufferSourceOptions {
    GC::Ptr<AudioBuffer> buffer;
    float detune { 0 };
    bool loop { false };
    double loop_end { 0 };
    double loop_start { 0 };
    float playback_rate { 1 };
};

// https://webaudio.github.io/web-audio-api/#AudioBufferSourceNode
class AudioBufferSourceNode : public AudioScheduledSourceNode {
    WEB_PLATFORM_OBJECT(AudioBufferSourceNode, AudioScheduledSourceNode);
    GC_DECLARE_ALLOCATOR(AudioBufferSourceNode);

public:
    virtual ~AudioBufferSourceNode() override;

    WebIDL::ExceptionOr<void> set_buffer(GC::Ptr<AudioBuffer>);
    GC::Ptr<AudioBuffer> buffer() const;
    GC::Ref<AudioParam> playback_rate() const;
    GC::Ref<AudioParam> detune() const;
    WebIDL::ExceptionOr<void> set_loop(bool);
    bool loop() const;
    WebIDL::ExceptionOr<void> set_loop_start(double);
    double loop_start() const;
    WebIDL::ExceptionOr<void> set_loop_end(double);
    double loop_end() const;
    WebIDL::UnsignedLong number_of_inputs() override { return 0; }
    WebIDL::UnsignedLong number_of_outputs() override { return 2; }

    WebIDL::ExceptionOr<void> start(Optional<double>, Optional<double>, Optional<double>);

    static WebIDL::ExceptionOr<GC::Ref<AudioBufferSourceNode>> create(JS::Realm&, GC::Ref<BaseAudioContext>, AudioBufferSourceOptions const& = {});
    static WebIDL::ExceptionOr<GC::Ref<AudioBufferSourceNode>> construct_impl(JS::Realm&, GC::Ref<BaseAudioContext>, AudioBufferSourceOptions const& = {});

protected:
    AudioBufferSourceNode(JS::Realm&, GC::Ref<BaseAudioContext>, AudioBufferSourceOptions const& = {});

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    GC::Ptr<AudioBuffer> m_buffer;
    GC::Ref<AudioParam> m_playback_rate;
    GC::Ref<AudioParam> m_detune;
    bool m_loop { false };
    double m_loop_start { 0.0 };
    double m_loop_end { 0.0 };
};

}
