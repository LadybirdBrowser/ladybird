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
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebAudio/AnalyserNode.h>
#include <LibWeb/WebAudio/AudioBuffer.h>
#include <LibWeb/WebAudio/AudioBufferSourceNode.h>
#include <LibWeb/WebAudio/AudioDestinationNode.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/BiquadFilterNode.h>
#include <LibWeb/WebAudio/ChannelMergerNode.h>
#include <LibWeb/WebAudio/DynamicsCompressorNode.h>
#include <LibWeb/WebAudio/GainNode.h>
#include <LibWeb/WebAudio/OscillatorNode.h>
#include <LibWeb/WebAudio/PannerNode.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::WebAudio {

BaseAudioContext::BaseAudioContext(JS::Realm& realm, float sample_rate)
    : DOM::EventTarget(realm)
    , m_sample_rate(sample_rate)
    , m_listener(AudioListener::create(realm, *this))
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
    visitor.visit(m_pending_promises);
    visitor.visit(m_listener);
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

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createstereopanner
WebIDL::ExceptionOr<GC::Ref<StereoPannerNode>> BaseAudioContext::create_stereo_panner()
{
    // Factory method for a StereoPannerNode.
    return StereoPannerNode::create(realm(), *this);
}

WebIDL::ExceptionOr<void> BaseAudioContext::verify_audio_options_inside_nominal_range(JS::Realm& realm, float sample_rate)
{
    if (sample_rate < MIN_SAMPLE_RATE || sample_rate > MAX_SAMPLE_RATE)
        return WebIDL::NotSupportedError::create(realm, "Sample rate is outside of allowed range"_string);

    return {};
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

    TRY(verify_audio_options_inside_nominal_range(realm, sample_rate));

    return {};
}

void BaseAudioContext::queue_a_media_element_task(GC::Ref<GC::Function<void()>> steps)
{
    auto task = HTML::Task::create(vm(), m_media_element_event_task_source.source, HTML::current_principal_settings_object().responsible_document(), steps);
    HTML::main_thread_event_loop().task_queue().add(task);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-decodeaudiodata
GC::Ref<WebIDL::Promise> BaseAudioContext::decode_audio_data(GC::Root<WebIDL::BufferSource> audio_data, GC::Ptr<WebIDL::CallbackType> success_callback, GC::Ptr<WebIDL::CallbackType> error_callback)
{
    auto& realm = this->realm();

    // FIXME: When decodeAudioData is called, the following steps MUST be performed on the control thread:

    // 1. If this's relevant global object's associated Document is not fully active then return a
    //    promise rejected with "InvalidStateError" DOMException.
    auto const& associated_document = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();
    if (!associated_document.is_fully_active()) {
        auto error = WebIDL::InvalidStateError::create(realm, "The document is not fully active."_string);
        return WebIDL::create_rejected_promise_from_exception(realm, error);
    }

    // 2. Let promise be a new Promise.
    auto promise = WebIDL::create_promise(realm);

    // FIXME: 3. If audioData is detached, execute the following steps:
    if (true) {
        // 3.1. Append promise to [[pending promises]].
        m_pending_promises.append(promise);

        // FIXME: 3.2. Detach the audioData ArrayBuffer. If this operations throws, jump to the step 3.

        // 3.3. Queue a decoding operation to be performed on another thread.
        queue_a_decoding_operation(promise, move(audio_data), success_callback, error_callback);
    }

    // 4. Else, execute the following error steps:
    else {
        // 4.1. Let error be a DataCloneError.
        auto error = WebIDL::DataCloneError::create(realm, "Audio data is not detached."_string);

        // 4.2. Reject promise with error, and remove it from [[pending promises]].
        WebIDL::reject_promise(realm, promise, error);
        m_pending_promises.remove_first_matching([&promise](auto& pending_promise) {
            return pending_promise == promise;
        });

        // 4.3. Queue a media element task to invoke errorCallback with error.
        if (error_callback) {
            queue_a_media_element_task(GC::create_function(heap(), [&realm, error_callback, error] {
                auto completion = WebIDL::invoke_callback(*error_callback, {}, error);
                if (completion.is_abrupt())
                    HTML::report_exception(completion, realm);
            }));
        }
    }

    // 5. Return promise.
    return promise;
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-decodeaudiodata
void BaseAudioContext::queue_a_decoding_operation(GC::Ref<JS::PromiseCapability> promise, [[maybe_unused]] GC::Root<WebIDL::BufferSource> audio_data, GC::Ptr<WebIDL::CallbackType> success_callback, GC::Ptr<WebIDL::CallbackType> error_callback)
{
    auto& realm = this->realm();

    // FIXME: When queuing a decoding operation to be performed on another thread, the following steps
    //        MUST happen on a thread that is not the control thread nor the rendering thread, called
    //        the decoding thread.

    // 1. Let can decode be a boolean flag, initially set to true.
    auto can_decode { true };

    // FIXME: 2. Attempt to determine the MIME type of audioData, using MIME Sniffing § 6.2 Matching an
    //           audio or video type pattern. If the audio or video type pattern matching algorithm returns
    //           undefined, set can decode to false.

    // 3. If can decode is true,
    if (can_decode) {
        // FIXME: attempt to decode the encoded audioData into linear PCM. In case of
        //        failure, set can decode to false.

        // FIXME: If the media byte-stream contains multiple audio tracks, only decode the first track to linear pcm.
    }

    // 4. If can decode is false,
    if (!can_decode) {
        // queue a media element task to execute the following steps:
        queue_a_media_element_task(GC::create_function(heap(), [this, &realm, promise, error_callback] {
            // 4.1. Let error be a DOMException whose name is EncodingError.
            auto error = WebIDL::EncodingError::create(realm, "Unable to decode."_string);

            // 4.1.2. Reject promise with error, and remove it from [[pending promises]].
            WebIDL::reject_promise(realm, promise, error);
            m_pending_promises.remove_first_matching([&promise](auto& pending_promise) {
                return pending_promise == promise;
            });

            // 4.2. If errorCallback is not missing, invoke errorCallback with error.
            if (error_callback) {
                auto completion = WebIDL::invoke_callback(*error_callback, {}, error);
                if (completion.is_abrupt())
                    HTML::report_exception(completion, realm);
            }
        }));
    }

    // 5. Otherwise:
    else {
        // FIXME: 5.1. Take the result, representing the decoded linear PCM audio data, and resample it to the
        //             sample-rate of the BaseAudioContext if it is different from the sample-rate of
        //             audioData.

        // FIXME: 5.2. queue a media element task to execute the following steps:

        // FIXME: 5.2.1. Let buffer be an AudioBuffer containing the final result (after possibly performing
        //               sample-rate conversion).
        auto buffer = MUST(create_buffer(2, 1, 44100));

        // 5.2.2. Resolve promise with buffer.
        WebIDL::resolve_promise(realm, promise, buffer);

        // 5.2.3. If successCallback is not missing, invoke successCallback with buffer.
        if (success_callback) {
            auto completion = WebIDL::invoke_callback(*success_callback, {}, buffer);
            if (completion.is_abrupt())
                HTML::report_exception(completion, realm);
        }
    }
}

}
