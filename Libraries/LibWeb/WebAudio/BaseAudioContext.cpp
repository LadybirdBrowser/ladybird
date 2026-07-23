/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibGC/Root.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibThreading/ThreadPool.h>
#include <LibWeb/Bindings/BaseAudioContext.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentObserver.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebAudio/AnalyserNode.h>
#include <LibWeb/WebAudio/AudioArray.h>
#include <LibWeb/WebAudio/AudioBuffer.h>
#include <LibWeb/WebAudio/AudioBufferSourceNode.h>
#include <LibWeb/WebAudio/AudioDestinationNode.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/BiquadFilterNode.h>
#include <LibWeb/WebAudio/ChannelMergerNode.h>
#include <LibWeb/WebAudio/ControlMessageQueue.h>
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
    , m_control_message_queue(make_ref_counted<ControlMessageQueue>())
{
}

BaseAudioContext::~BaseAudioContext() = default;

void BaseAudioContext::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(BaseAudioContext);
    Base::initialize(realm);

    if (auto* window = as_if<HTML::Window>(realm.global_object())) {
        // FIXME: Also handle the document becoming active again, e.g. when restored from the back/forward cache.
        m_document_observer = realm.create<DOM::DocumentObserver>(realm, window->associated_document());
        m_document_observer->set_document_became_inactive([this]() {
            document_became_inactive();
        });
    }
}

void BaseAudioContext::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_destination);
    visitor.visit(m_pending_promises);
    visitor.visit(m_listener);
    visitor.visit(m_playing_sources);
    visitor.visit(m_document_observer);
}

// https://webaudio.github.io/web-audio-api/#dom-audioscheduledsourcenode-onended
void BaseAudioContext::add_playing_source(GC::Ref<AudioNode> node)
{
    // NB: A started source node is kept alive until it stops playing, at which point an ended event is fired at it.
    m_playing_sources.set(node->node_id(), node);
}

void BaseAudioContext::handle_ended_sources(ReadonlySpan<NodeID> ended_nodes)
{
    for (auto node_id : ended_nodes) {
        auto node = m_playing_sources.take(node_id);
        if (!node.has_value())
            continue;
        queue_a_media_element_task(GC::create_function(heap(), [node = *node] {
            node->dispatch_event(DOM::Event::create(node->realm(), HTML::EventNames::ended));
        }));
    }
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
    Bindings::ChannelMergerOptions options;
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
    Bindings::DelayOptions options;
    options.max_delay_time = max_delay_time;

    return DelayNode::create(realm(), *this, options);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createchannelsplitter
WebIDL::ExceptionOr<GC::Ref<ChannelSplitterNode>> BaseAudioContext::create_channel_splitter(WebIDL::UnsignedLong number_of_outputs)
{
    Bindings::ChannelSplitterOptions options;
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

WebIDL::ExceptionOr<GC::Ref<PeriodicWave>> BaseAudioContext::create_periodic_wave(Vector<float> const& real, Vector<float> const& imag, Optional<Bindings::PeriodicWaveConstraints> const& constraints)
{
    Bindings::PeriodicWaveOptions options;
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

    return ScriptProcessorNode::create(realm(), *this, buffer_size, number_of_input_channels,
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
    auto task = HTML::Task::create(vm(), m_media_element_event_task_source.source, HTML::relevant_settings_object(*this).responsible_document(), steps);
    (void)HTML::main_thread_event_loop().task_queue().add(task);
}

void BaseAudioContext::queue_control_message(ControlMessage message)
{
    m_control_message_queue->enqueue(move(message));
    // FIXME: Should signal the rendering thread when implemented
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-decodeaudiodata
GC::Ref<WebIDL::Promise> BaseAudioContext::decode_audio_data(GC::Ref<JS::ArrayBuffer> audio_data, GC::Ptr<WebIDL::CallbackType> success_callback, GC::Ptr<WebIDL::CallbackType> error_callback)
{
    auto& realm = this->realm();

    // FIXME: When decodeAudioData is called, the following steps MUST be performed on the control thread:

    // 1. If this's relevant global object's associated Document is not fully active then return a
    //    promise rejected with "InvalidStateError" DOMException.
    auto const& associated_document = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();
    if (!associated_document.is_fully_active()) {
        auto error = WebIDL::InvalidStateError::create(realm, "The document is not fully active."_utf16);
        return WebIDL::create_rejected_promise_from_exception(realm, error);
    }

    // 2. Let promise be a new Promise.
    auto promise = WebIDL::create_promise(realm);

    // 3. If audioData is detached, execute the following steps:
    // NB: The spec condition is inverted: decoding proceeds when audioData is _not_ detached, and the error steps
    //     below reject buffers that already are. See https://github.com/WebAudio/web-audio-api/issues/2570
    auto queued_a_decoding_operation = false;
    if (!audio_data->is_detached()) {
        // 3.1. Append promise to [[pending promises]].
        m_pending_promises.append(promise);

        // 3.2. Detach the audioData ArrayBuffer. If this operations throws, jump to the step 3.
        // NB: The buffer's contents are copied out before detaching it, since detaching frees the data. "Jump to
        //     the step 3" is interpreted as running the error steps below.
        auto audio_data_copy = audio_data->copy_to_byte_buffer();
        auto detach_result = JS::detach_array_buffer(realm.vm(), audio_data);
        if (!audio_data_copy.is_error() && !detach_result.is_error()) {
            // 3.3. Queue a decoding operation to be performed on another thread.
            queue_a_decoding_operation(promise, audio_data_copy.release_value(), success_callback, error_callback);
            queued_a_decoding_operation = true;
        }
    }

    // 4. Else, execute the following error steps:
    if (!queued_a_decoding_operation) {
        // 4.1. Let error be a DataCloneError.
        auto error = WebIDL::DataCloneError::create(realm, "Cannot decode detached audio data."_utf16);

        // 4.2. Reject promise with error, and remove it from [[pending promises]].
        WebIDL::reject_promise(realm, promise, error);
        m_pending_promises.remove_first_matching([&promise](auto& pending_promise) {
            return pending_promise == promise;
        });

        // 4.3. Queue a media element task to invoke errorCallback with error.
        if (error_callback) {
            queue_a_media_element_task(GC::create_function(heap(), [&realm, error_callback, error] {
                auto completion = WebIDL::invoke_callback(*error_callback, {}, { { error } });
                if (completion.is_abrupt())
                    HTML::report_exception(completion, realm);
            }));
        }
    }

    // 5. Return promise.
    return promise;
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-decodeaudiodata
void BaseAudioContext::queue_a_decoding_operation(GC::Ref<JS::PromiseCapability> promise, ByteBuffer audio_data, GC::Ptr<WebIDL::CallbackType> success_callback, GC::Ptr<WebIDL::CallbackType> error_callback)
{
    // When queuing a decoding operation to be performed on another thread, the following steps MUST happen on a
    // thread that is not the control thread nor the rendering thread, called the decoding thread.
    // NB: Decoding operations are submitted to a thread pool, so multiple decoding threads may run in parallel. The
    //     GC roots below keep the context, promise and callbacks alive while the operation is in flight; they are
    //     created here on the control thread and released on the control thread again after the result is posted
    //     back through the main thread's event loop.
    auto& main_thread_event_loop = Core::EventLoop::current();

    // 5.1. Take the result, representing the decoded linear PCM audio data, and resample it to the sample-rate of
    //      the BaseAudioContext if it is different from the sample-rate of audioData.
    // NB: Resampling happens as part of the decoding operation below.
    // FIXME: Non-integral context sample rates cannot be represented by our audio converter; for those, we decode at
    //        the stream's native sample rate instead. This is fine for playback, since AudioBufferSourceNode
    //        resamples buffers whose rate differs from the context's, but it is observable through
    //        AudioBuffer.sampleRate.
    Optional<u32> context_sample_rate;
    if (m_sample_rate == static_cast<float>(static_cast<u32>(m_sample_rate)))
        context_sample_rate = static_cast<u32>(m_sample_rate);

    Threading::ThreadPool::the().submit([self = GC::make_root(*this), promise = GC::make_root(promise),
                                            success_callback = success_callback ? GC::make_root(*success_callback) : GC::Root<WebIDL::CallbackType> {},
                                            error_callback = error_callback ? GC::make_root(*error_callback) : GC::Root<WebIDL::CallbackType> {},
                                            audio_data = move(audio_data), context_sample_rate, &main_thread_event_loop] mutable {
        // 1. Let can decode be a boolean flag, initially set to true.
        // NB: Represented by decode_result below not holding an error.

        // FIXME: 2. Attempt to determine the MIME type of audioData, using MIME Sniffing § 6.2 Matching an
        //           audio or video type pattern. If the audio or video type pattern matching algorithm returns
        //           undefined, set can decode to false.

        // 3. If can decode is true, attempt to decode the encoded audioData into linear PCM. In case of failure,
        //    set can decode to false.
        //    If the media byte-stream contains multiple audio tracks, only decode the first track to linear pcm.
        auto decode_result = [&]() -> Media::DecoderErrorOr<Media::DecodedAudioData> {
            if (audio_data.is_empty())
                return Media::DecoderError::with_description(Media::DecoderErrorCategory::Corrupted, "Audio data is empty"sv);
            auto stream = Media::IncrementallyPopulatedStream::create_from_data(audio_data);
            return Media::decode_entire_audio_stream(move(stream), context_sample_rate);
        }();

        main_thread_event_loop.deferred_invoke([self = move(self), promise = move(promise), success_callback = move(success_callback),
                                                   error_callback = move(error_callback), decode_result = move(decode_result)] mutable {
            self->finish_a_decoding_operation(*promise, success_callback.ptr(), error_callback.ptr(), move(decode_result));
        });
    });
}

// Continuation of the decoding operation above, back on the control thread with the decoding thread's result.
void BaseAudioContext::finish_a_decoding_operation(GC::Ref<JS::PromiseCapability> promise, GC::Ptr<WebIDL::CallbackType> success_callback, GC::Ptr<WebIDL::CallbackType> error_callback, Media::DecoderErrorOr<Media::DecodedAudioData> decode_result)
{
    auto reject_with_encoding_error = [this, promise, error_callback] {
        // 4. If can decode is false, queue a media element task to execute the following steps:
        queue_a_media_element_task(GC::create_function(heap(), [this, promise, error_callback] {
            auto& realm = this->realm();
            HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

            // 4.1. Let error be a DOMException whose name is EncodingError.
            auto error = WebIDL::EncodingError::create(realm, "Unable to decode."_utf16);

            // 4.1.2. Reject promise with error, and remove it from [[pending promises]].
            WebIDL::reject_promise(realm, promise, error);
            m_pending_promises.remove_first_matching([&promise](auto& pending_promise) {
                return pending_promise == promise;
            });

            // 4.2. If errorCallback is not missing, invoke errorCallback with error.
            if (error_callback) {
                auto completion = WebIDL::invoke_callback(*error_callback, {}, { { error } });
                if (completion.is_abrupt())
                    HTML::report_exception(completion, realm);
            }
        }));
    };

    if (decode_result.is_error()) {
        reject_with_encoding_error();
        return;
    }

    // 5. Otherwise:
    // 5.2. queue a media element task to execute the following steps:
    queue_a_media_element_task(GC::create_function(heap(), [this, promise, success_callback, reject_with_encoding_error = move(reject_with_encoding_error), data = decode_result.release_value()] {
        auto& realm = this->realm();
        HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);

        // 5.2.1. Let buffer be an AudioBuffer containing the final result (after possibly performing sample-rate
        //        conversion).
        // NB: Buffer creation fails for decoded audio that falls outside AudioBuffer's supported ranges, e.g. too
        //     many channels; treat that the same as a decoding failure.
        auto length = data.channels[0].size();
        if (length > NumericLimits<WebIDL::UnsignedLong>::max()) {
            reject_with_encoding_error();
            return;
        }
        auto buffer_result = AudioBuffer::create(realm, static_cast<WebIDL::UnsignedLong>(data.channels.size()), static_cast<WebIDL::UnsignedLong>(length), static_cast<float>(data.sample_specification.sample_rate()));
        if (buffer_result.is_error()) {
            reject_with_encoding_error();
            return;
        }
        auto buffer = buffer_result.release_value();
        for (size_t channel = 0; channel < data.channels.size(); channel++)
            overwrite_float32_array(MUST(buffer->get_channel_data(channel)), data.channels[channel]);

        // 5.2.2. Resolve promise with buffer.
        WebIDL::resolve_promise(realm, promise, buffer);

        // 5.2.3. If successCallback is not missing, invoke successCallback with buffer.
        if (success_callback) {
            auto completion = WebIDL::invoke_callback(*success_callback, {}, { { buffer } });
            if (completion.is_abrupt())
                HTML::report_exception(completion, realm);
        }
    }));
}

}
