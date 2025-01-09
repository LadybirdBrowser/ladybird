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

GC_DEFINE_ALLOCATOR(AudioBufferSourceNode);

AudioBufferSourceNode::AudioBufferSourceNode(JS::Realm& realm, GC::Ref<BaseAudioContext> context, AudioBufferSourceOptions const& options)
    : AudioScheduledSourceNode(realm, context)
    , m_buffer(options.buffer)
    , m_playback_rate(AudioParam::create(realm, context, options.playback_rate, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
    , m_detune(AudioParam::create(realm, context, options.detune, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
    , m_loop(options.loop)
    , m_loop_start(options.loop_start)
    , m_loop_end(options.loop_end)
{
}

AudioBufferSourceNode::~AudioBufferSourceNode() = default;

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-buffer
WebIDL::ExceptionOr<void> AudioBufferSourceNode::set_buffer(GC::Ptr<AudioBuffer> buffer)
{
    // 1. Let new buffer be the AudioBuffer or null value to be assigned to buffer.
    auto new_buffer = buffer;

    // 2. If new buffer is not null and [[buffer set]] is true, throw an InvalidStateError and abort these steps.
    if (new_buffer && m_buffer_set)
        return WebIDL::InvalidStateError::create(realm(), "Buffer has already been set"_string);

    // 3. If new buffer is not null, set [[buffer set]] to true.
    if (new_buffer)
        m_buffer_set = true;

    // 4. Assign new buffer to the buffer attribute.
    m_buffer = new_buffer;

    // FIXME: 5. If start() has previously been called on this node, perform the operation acquire the content on buffer.

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-buffer
GC::Ptr<AudioBuffer> AudioBufferSourceNode::buffer() const
{
    return m_buffer;
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-playbackrate
GC::Ref<AudioParam> AudioBufferSourceNode::playback_rate() const
{
    return m_playback_rate;
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-detune
GC::Ref<AudioParam> AudioBufferSourceNode::detune() const
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
    // 1. If this AudioBufferSourceNode internal slot [[source started]] is true, an InvalidStateError exception MUST be thrown.
    if (source_started())
        return WebIDL::InvalidStateError::create(realm(), "AudioBufferSourceNode has already been started"_string);

    // 2. Check for any errors that must be thrown due to parameter constraints described below. If any exception is thrown during this step, abort those steps.
    // A RangeError exception MUST be thrown if when is negative.
    if (when.has_value() && when.value() < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "when must not be negative"sv };

    // A RangeError exception MUST be thrown if offset is negative
    if (offset.has_value() && offset.value() < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "offset must not be negative"sv };

    // A RangeError exception MUST be thrown if duration is negative.
    if (duration.has_value() && duration.value() < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "duration must not be negative"sv };

    // 3. Set the internal slot [[source started]] on this AudioBufferSourceNode to true.
    set_source_started(true);

    // FIXME: 4. Queue a control message to start the AudioBufferSourceNode, including the parameter values in the message.
    // FIXME: 5. Acquire the contents of the buffer if the buffer has been set.
    // FIXME: 6. Send a control message to the associated AudioContext to start running its rendering thread only when all the following conditions are met:

    dbgln("FIXME: Implement AudioBufferSourceNode::start(when, offset, duration)");
    return {};
}

WebIDL::ExceptionOr<GC::Ref<AudioBufferSourceNode>> AudioBufferSourceNode::create(JS::Realm& realm, GC::Ref<BaseAudioContext> context, AudioBufferSourceOptions const& options)
{
    return construct_impl(realm, context, options);
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-audiobuffersourcenode
WebIDL::ExceptionOr<GC::Ref<AudioBufferSourceNode>> AudioBufferSourceNode::construct_impl(JS::Realm& realm, GC::Ref<BaseAudioContext> context, AudioBufferSourceOptions const& options)
{
    // When the constructor is called with a BaseAudioContext c and an option object option, the user agent
    // MUST initialize the AudioNode this, with context and options as arguments.

    auto node = realm.create<AudioBufferSourceNode>(realm, context, options);

    // Default options for channel count and interpretation
    // https://webaudio.github.io/web-audio-api/#AudioBufferSourceNode
    AudioNodeDefaultOptions default_options;
    default_options.channel_count = 2;
    default_options.channel_count_mode = Bindings::ChannelCountMode::Max;
    default_options.channel_interpretation = Bindings::ChannelInterpretation::Speakers;
    // FIXME: Set tail-time to no

    TRY(node->initialize_audio_node_options(options, default_options));

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
