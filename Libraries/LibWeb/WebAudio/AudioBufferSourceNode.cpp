/*
 * Copyright (c) 2024, Bar Yemini <bar.ye651@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/AudioScheduledSourceNodePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioBuffer.h>
#include <LibWeb/WebAudio/AudioBufferSourceNode.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/AudioScheduledSourceNode.h>

namespace Web::WebAudio {

JS_DEFINE_ALLOCATOR(AudioBufferSourceNode);

AudioBufferSourceNode::AudioBufferSourceNode(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext> context, AudioBufferSourceOptions const& options)
    : AudioScheduledSourceNode(realm, context)
    , m_buffer(options.buffer)
    , m_playback_rate(AudioParam::create(realm, options.playback_rate, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
    , m_detune(AudioParam::create(realm, options.detune, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
    , m_loop(options.loop)
    , m_loop_start(options.loop_start)
    , m_loop_end(options.loop_end)
{
}

AudioBufferSourceNode::~AudioBufferSourceNode() = default;

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-buffer
WebIDL::ExceptionOr<void> AudioBufferSourceNode::set_buffer(JS::GCPtr<AudioBuffer> buffer)
{
    m_buffer = buffer;
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-buffer
JS::GCPtr<AudioBuffer> AudioBufferSourceNode::buffer() const
{
    return m_buffer;
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-playbackrate
JS::NonnullGCPtr<AudioParam> AudioBufferSourceNode::playback_rate() const
{
    return m_playback_rate;
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-detune
JS::NonnullGCPtr<AudioParam> AudioBufferSourceNode::detune() const
{
    return m_detune;
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-loop
WebIDL::ExceptionOr<void> AudioBufferSourceNode::set_loop(bool loop)
{
    m_loop = loop;
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-loop
bool AudioBufferSourceNode::loop() const
{
    return m_loop;
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-loopstart
WebIDL::ExceptionOr<void> AudioBufferSourceNode::set_loop_start(double loop_start)
{
    m_loop_start = loop_start;
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-loopstart
double AudioBufferSourceNode::loop_start() const
{
    return m_loop_start;
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-loopend
WebIDL::ExceptionOr<void> AudioBufferSourceNode::set_loop_end(double loop_end)
{
    m_loop_end = loop_end;
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-loopend
double AudioBufferSourceNode::loop_end() const
{
    return m_loop_end;
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-start`
WebIDL::ExceptionOr<void> AudioBufferSourceNode::start(Optional<double> when, Optional<double> offset, Optional<double> duration)
{
    (void)when;
    (void)offset;
    (void)duration;
    dbgln("FIXME: Implement AudioBufferSourceNode::start(when, offset, duration)");
    return {};
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioBufferSourceNode>> AudioBufferSourceNode::create(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext> context, AudioBufferSourceOptions const& options)
{
    return construct_impl(realm, context, options);
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-audiobuffersourcenode
WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioBufferSourceNode>> AudioBufferSourceNode::construct_impl(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext> context, AudioBufferSourceOptions const& options)
{
    // When the constructor is called with a BaseAudioContext c and an option object option, the user agent
    // MUST initialize the AudioNode this, with context and options as arguments.

    auto node = realm.vm().heap().allocate<AudioBufferSourceNode>(realm, realm, context, options);
    return node;
}

void AudioBufferSourceNode::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AudioBufferSourceNode);
}

void AudioBufferSourceNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_buffer);
    visitor.visit(m_playback_rate);
    visitor.visit(m_detune);
}

}
