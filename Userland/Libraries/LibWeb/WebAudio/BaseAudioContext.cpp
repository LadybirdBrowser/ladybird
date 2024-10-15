/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/BaseAudioContextPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/WebAudio/AudioBuffer.h>
#include <LibWeb/WebAudio/AudioBufferSourceNode.h>
#include <LibWeb/WebAudio/AudioDestinationNode.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/BiquadFilterNode.h>
#include <LibWeb/WebAudio/DynamicsCompressorNode.h>
#include <LibWeb/WebAudio/GainNode.h>
#include <LibWeb/WebAudio/OscillatorNode.h>

namespace Web::WebAudio {

BaseAudioContext::BaseAudioContext(JS::Realm& realm, float sample_rate)
    : DOM::EventTarget(realm)
    , m_destination(AudioDestinationNode::construct_impl(realm, *this))
    , m_sample_rate(sample_rate)
{
}

BaseAudioContext::~BaseAudioContext() = default;

void BaseAudioContext::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(BaseAudioContext);
}

void BaseAudioContext::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_destination);
}

void BaseAudioContext::set_onstatechange(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::statechange, event_handler);
}

WebIDL::CallbackType* BaseAudioContext::onstatechange()
{
    return event_handler_attribute(HTML::EventNames::statechange);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createbiquadfilter
WebIDL::ExceptionOr<JS::NonnullGCPtr<BiquadFilterNode>> BaseAudioContext::create_biquad_filter()
{
    // Factory method for a BiquadFilterNode representing a second order filter which can be configured as one of several common filter types.
    return BiquadFilterNode::create(realm(), *this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createbuffer
WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioBuffer>> BaseAudioContext::create_buffer(WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate)
{
    // Creates an AudioBuffer of the given size. The audio data in the buffer will be zero-initialized (silent).
    // A NotSupportedError exception MUST be thrown if any of the arguments is negative, zero, or outside its nominal range.
    return AudioBuffer::create(realm(), number_of_channels, length, sample_rate);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createbuffersource
WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioBufferSourceNode>> BaseAudioContext::create_buffer_source()
{
    // Factory method for a AudioBufferSourceNode.
    return AudioBufferSourceNode::create(realm(), *this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createoscillator
WebIDL::ExceptionOr<JS::NonnullGCPtr<OscillatorNode>> BaseAudioContext::create_oscillator()
{
    // Factory method for an OscillatorNode.
    return OscillatorNode::create(realm(), *this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createdynamicscompressor
WebIDL::ExceptionOr<JS::NonnullGCPtr<DynamicsCompressorNode>> BaseAudioContext::create_dynamics_compressor()
{
    // Factory method for a DynamicsCompressorNode.
    return DynamicsCompressorNode::create(realm(), *this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-creategain
JS::NonnullGCPtr<GainNode> BaseAudioContext::create_gain()
{
    // Factory method for GainNode.
    return GainNode::create(realm(), *this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createbuffer
WebIDL::ExceptionOr<void> BaseAudioContext::verify_audio_options_inside_nominal_range(JS::Realm& realm, WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate)
{
    // A NotSupportedError exception MUST be thrown if any of the arguments is negative, zero, or outside its nominal range.

    if (number_of_channels == 0)
        return WebIDL::NotSupportedError::create(realm, "Number of channels must not be '0'"_string);

    if (number_of_channels > MAX_NUMBER_OF_CHANNELS)
        return WebIDL::NotSupportedError::create(realm, "Number of channels is greater than allowed range"_string);

    if (length == 0)
        return WebIDL::NotSupportedError::create(realm, "Length of buffer must be at least 1"_string);

    if (sample_rate < MIN_SAMPLE_RATE || sample_rate > MAX_SAMPLE_RATE)
        return WebIDL::NotSupportedError::create(realm, "Sample rate is outside of allowed range"_string);

    return {};
}

void BaseAudioContext::queue_a_media_element_task(JS::NonnullGCPtr<JS::HeapFunction<void()>> steps)
{
    auto task = HTML::Task::create(vm(), m_media_element_event_task_source.source, HTML::current_settings_object().responsible_document(), steps);
    HTML::main_thread_event_loop().task_queue().add(task);
}

}
