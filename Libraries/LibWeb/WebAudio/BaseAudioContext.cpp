/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebAudio/AnalyserNode.h>
#include <LibWeb/WebAudio/AudioBuffer.h>
#include <LibWeb/WebAudio/AudioBufferSourceNode.h>
#include <LibWeb/WebAudio/AudioDestinationNode.h>
#include <LibWeb/WebAudio/BackgroundAudioDecoder.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/BiquadFilterNode.h>
#include <LibWeb/WebAudio/ChannelMergerNode.h>
#include <LibWeb/WebAudio/ControlMessageQueue.h>
#include <LibWeb/WebAudio/DynamicsCompressorNode.h>
#include <LibWeb/WebAudio/GainNode.h>
#include <LibWeb/WebAudio/OscillatorNode.h>
#include <LibWeb/WebAudio/PannerNode.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::WebAudio {

BaseAudioContext::BaseAudioContext(JS::Realm& realm, float sample_rate)
    : DOM::EventTarget(realm)
    , m_sample_rate(sample_rate)
    , m_listener(AudioListener::create(realm, *this))
    , m_control_message_queue(make<ControlMessageQueue>())
{
}

BaseAudioContext::~BaseAudioContext() = default;

void BaseAudioContext::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(BaseAudioContext);
    Base::initialize(realm);
}

void BaseAudioContext::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_destination);
    visitor.visit(m_pending_promises);
    visitor.visit(m_listener);
}

bool BaseAudioContext::take_pending_promise(GC::Ref<WebIDL::Promise> const& promise)
{
    return m_pending_promises.remove_first_matching([&](auto& pending_promise) {
        return pending_promise == promise;
    });
}

void BaseAudioContext::set_onstatechange(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::statechange, event_handler);
}

WebIDL::CallbackType* BaseAudioContext::onstatechange()
{
    return event_handler_attribute(HTML::EventNames::statechange);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createanalyser
WebIDL::ExceptionOr<GC::Ref<AnalyserNode>> BaseAudioContext::create_analyser()
{
    return AnalyserNode::create(realm(), *this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createbiquadfilter
WebIDL::ExceptionOr<GC::Ref<BiquadFilterNode>> BaseAudioContext::create_biquad_filter()
{
    // Factory method for a BiquadFilterNode representing a second order filter which can be configured as one of several common filter types.
    return BiquadFilterNode::create(realm(), *this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createbuffer
WebIDL::ExceptionOr<GC::Ref<AudioBuffer>> BaseAudioContext::create_buffer(WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate)
{
    // Creates an AudioBuffer of the given size. The audio data in the buffer will be zero-initialized (silent).
    // A NotSupportedError exception MUST be thrown if any of the arguments is negative, zero, or outside its nominal range.
    return AudioBuffer::create(realm(), number_of_channels, length, sample_rate);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createbuffersource
WebIDL::ExceptionOr<GC::Ref<AudioBufferSourceNode>> BaseAudioContext::create_buffer_source()
{
    // Factory method for a AudioBufferSourceNode.
    return AudioBufferSourceNode::create(realm(), *this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createchannelmerger
WebIDL::ExceptionOr<GC::Ref<ChannelMergerNode>> BaseAudioContext::create_channel_merger(WebIDL::UnsignedLong number_of_inputs)
{
    ChannelMergerOptions options;
    options.number_of_inputs = number_of_inputs;

    return ChannelMergerNode::create(realm(), *this, options);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createconstantsource
WebIDL::ExceptionOr<GC::Ref<ConstantSourceNode>> BaseAudioContext::create_constant_source()
{
    return ConstantSourceNode::create(realm(), *this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createdelay
WebIDL::ExceptionOr<GC::Ref<DelayNode>> BaseAudioContext::create_delay(double max_delay_time)
{
    DelayOptions options;
    options.max_delay_time = max_delay_time;

    return DelayNode::create(realm(), *this, options);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createchannelsplitter
WebIDL::ExceptionOr<GC::Ref<ChannelSplitterNode>> BaseAudioContext::create_channel_splitter(WebIDL::UnsignedLong number_of_outputs)
{
    ChannelSplitterOptions options;
    options.number_of_outputs = number_of_outputs;

    return ChannelSplitterNode::create(realm(), *this, options);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createoscillator
WebIDL::ExceptionOr<GC::Ref<OscillatorNode>> BaseAudioContext::create_oscillator()
{
    // Factory method for an OscillatorNode.
    return OscillatorNode::create(realm(), *this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createdynamicscompressor
WebIDL::ExceptionOr<GC::Ref<DynamicsCompressorNode>> BaseAudioContext::create_dynamics_compressor()
{
    // Factory method for a DynamicsCompressorNode.
    return DynamicsCompressorNode::create(realm(), *this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-creategain
WebIDL::ExceptionOr<GC::Ref<GainNode>> BaseAudioContext::create_gain()
{
    // Factory method for GainNode.
    return GainNode::create(realm(), *this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createpanner
WebIDL::ExceptionOr<GC::Ref<PannerNode>> BaseAudioContext::create_panner()
{
    // Factory method for a PannerNode.
    return PannerNode::create(realm(), *this);
}

WebIDL::ExceptionOr<GC::Ref<PeriodicWave>> BaseAudioContext::create_periodic_wave(Vector<float> const& real, Vector<float> const& imag, Optional<PeriodicWaveConstraints> const& constraints)
{
    PeriodicWaveOptions options;
    options.real = real;
    options.imag = imag;
    if (constraints.has_value())
        options.disable_normalization = constraints->disable_normalization;

    return PeriodicWave::construct_impl(realm(), *this, options);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createscriptprocessor
WebIDL::ExceptionOr<GC::Ref<ScriptProcessorNode>> BaseAudioContext::create_script_processor(
    WebIDL::UnsignedLong buffer_size,
    WebIDL::UnsignedLong number_of_input_channels,
    WebIDL::UnsignedLong number_of_output_channels)
{
    // The bufferSize parameter determines the buffer size in units of sample-frames. If it’s not passed in, or if the
    // value is 0, then the implementation will choose the best buffer size for the given environment, which will be
    // constant power of 2 throughout the lifetime of the node.
    if (buffer_size == 0)
        buffer_size = ScriptProcessorNode::DEFAULT_BUFFER_SIZE;

    return ScriptProcessorNode::create(realm(), *this, static_cast<WebIDL::Long>(buffer_size), number_of_input_channels,
        number_of_output_channels);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createstereopanner
WebIDL::ExceptionOr<GC::Ref<StereoPannerNode>> BaseAudioContext::create_stereo_panner()
{
    // Factory method for a StereoPannerNode.
    return StereoPannerNode::create(realm(), *this);
}

WebIDL::ExceptionOr<void> BaseAudioContext::verify_audio_options_inside_nominal_range(JS::Realm& realm, float sample_rate)
{
    if (sample_rate < MIN_SAMPLE_RATE || sample_rate > MAX_SAMPLE_RATE)
        return WebIDL::NotSupportedError::create(realm, "Sample rate is outside of allowed range"_utf16);

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createbuffer
WebIDL::ExceptionOr<void> BaseAudioContext::verify_audio_options_inside_nominal_range(JS::Realm& realm, WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate)
{
    // A NotSupportedError exception MUST be thrown if any of the arguments is negative, zero, or outside its nominal range.

    if (number_of_channels == 0)
        return WebIDL::NotSupportedError::create(realm, "Number of channels must not be '0'"_utf16);

    if (number_of_channels > MAX_NUMBER_OF_CHANNELS)
        return WebIDL::NotSupportedError::create(realm, "Number of channels is greater than allowed range"_utf16);

    if (length == 0)
        return WebIDL::NotSupportedError::create(realm, "Length of buffer must be at least 1"_utf16);

    TRY(verify_audio_options_inside_nominal_range(realm, sample_rate));

    return {};
}

void BaseAudioContext::queue_a_media_element_task(GC::Ref<GC::Function<void()>> steps)
{
    auto const& associated_document = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();
    auto task = HTML::Task::create(vm(), m_media_element_event_task_source.source, associated_document, steps);
    HTML::main_thread_event_loop().task_queue().add(task);
}

void BaseAudioContext::queue_control_message(ControlMessage message)
{
    m_control_message_queue->enqueue(move(message));
    // FIXME: Should signal the rendering thread when implemented
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-decodeaudiodata
GC::Ref<WebIDL::Promise> BaseAudioContext::decode_audio_data(GC::Root<WebIDL::BufferSource> const& audio_data, GC::Ptr<WebIDL::CallbackType> success_callback, GC::Ptr<WebIDL::CallbackType> error_callback)
{
    auto& associated_document = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();
    return associated_document.background_audio_decoder().decode_audio_data(*this, audio_data, success_callback, error_callback);
}

}
