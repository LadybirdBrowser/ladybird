/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/VM.h>
#include <LibMedia/DecodeAudioStream.h>
#include <LibMedia/IncrementallyPopulatedStream.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentObserver.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/ExceptionReporter.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
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
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOrUtils.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::WebAudio {

static JS::Value audio_buffer(JS::Realm& realm, GC::Ptr<AudioBuffer> buffer)
{
    if (!buffer)
        return JS::js_null();
    return Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, buffer);
}

static void invoke_audio_buffer_callback(JS::Realm& realm, WebIDL::CallbackType& callback, AudioBuffer& buffer)
{
    auto completion = WebIDL::invoke_callback(callback, {}, { { audio_buffer(realm, buffer) } });
    if (completion.is_abrupt())
        HTML::report_exception(completion, realm);
}

static void invoke_error_callback(JS::Realm& realm, WebIDL::CallbackType& callback, WebIDL::DOMException& error)
{
    auto completion = WebIDL::invoke_callback(callback, {}, { { throw_completion(realm, error).value() } });
    if (completion.is_abrupt())
        HTML::report_exception(completion, realm);
}

void resolve_audio_buffer_promise(JS::Realm& realm, WebIDL::Promise const& promise, AudioBuffer& buffer)
{
    WebIDL::resolve_promise(promise, audio_buffer(realm, buffer));
}

BaseAudioContext::BaseAudioContext(GC::Ref<DOM::EventTarget> relevant_global_object, float sample_rate)
    : DOM::EventTarget()
    , m_sample_rate(sample_rate)
    , m_global_object(relevant_global_object)
    , m_control_message_queue(adopt_ref(*new ControlMessageQueue))
{
    if (auto* window = as_if<HTML::Window>(*m_global_object)) {
        // FIXME: Also handle the document becoming active again, e.g. when restored from the back/forward cache.
        m_document_observer = DOM::DocumentObserver::create(window->associated_document());
        m_document_observer->set_document_became_inactive([this]() {
            document_became_inactive();
        });
    }
}

BaseAudioContext::~BaseAudioContext() = default;

void BaseAudioContext::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_destination);
    visitor.visit(m_pending_promises);
    visitor.visit(m_listener);
    visitor.visit(m_global_object);
    visitor.visit(m_document_observer);
    visitor.visit(m_playing_sources);
}

void BaseAudioContext::add_playing_source(GC::Ref<AudioNode> node)
{
    m_playing_sources.set(node->node_id(), node);
}

void BaseAudioContext::handle_ended_sources(ReadonlySpan<NodeID> ended_nodes)
{
    for (auto node_id : ended_nodes) {
        auto node = m_playing_sources.take(node_id);
        if (!node.has_value())
            continue;
        queue_a_media_element_task(GC::create_function(heap(), [node = *node] {
            node->dispatch_event(DOM::Event::create(node->relevant_global_object(), HTML::EventNames::ended));
        }));
    }
}

HTML::EnvironmentSettingsObject& BaseAudioContext::relevant_settings_object() const
{
    return HTML::relevant_settings_object(HTML::relevant_window_or_worker_global_scope(*m_global_object));
}

JS::Object& BaseAudioContext::relevant_global_object() const
{
    return HTML::relevant_global_object(HTML::relevant_window_or_worker_global_scope(*m_global_object));
}

GC::Ref<DOM::Event> BaseAudioContext::create_associated_event(Utf16FlyString const& event_name) const
{
    return DOM::Event::create(event_name,
        HighResolutionTime::current_high_resolution_time(relevant_global_object()));
}

HTML::Window& BaseAudioContext::relevant_window() const
{
    auto* window = HTML::window_from_global_object(relevant_global_object());
    VERIFY(window);
    return *window;
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
    return AnalyserNode::create(*this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createbiquadfilter
WebIDL::ExceptionOr<GC::Ref<BiquadFilterNode>> BaseAudioContext::create_biquad_filter()
{
    // Factory method for a BiquadFilterNode representing a second order filter which can be configured as one of several common filter types.
    return BiquadFilterNode::create(*this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createbuffer
WebIDL::ExceptionOr<GC::Ref<AudioBuffer>> BaseAudioContext::create_buffer(WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate)
{
    auto& vm = JS::VM::the();

    // Creates an AudioBuffer of the given size. The audio data in the buffer will be zero-initialized (silent).
    // A NotSupportedError exception MUST be thrown if any of the arguments is negative, zero, or outside its nominal range.
    TRY(verify_audio_options_inside_nominal_range(number_of_channels, length, sample_rate));

    if (length > NumericLimits<i32>::max() / sizeof(float))
        return vm.throw_completion<JS::RangeError>(JS::ErrorType::InvalidLength, "typed array");

    return TRY_OR_THROW_OOM(vm, AudioBuffer::create(number_of_channels, length, sample_rate));
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createbuffersource
WebIDL::ExceptionOr<GC::Ref<AudioBufferSourceNode>> BaseAudioContext::create_buffer_source()
{
    // Factory method for a AudioBufferSourceNode.
    return AudioBufferSourceNode::create(*this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createchannelmerger
WebIDL::ExceptionOr<GC::Ref<ChannelMergerNode>> BaseAudioContext::create_channel_merger(WebIDL::UnsignedLong number_of_inputs)
{
    ChannelMergerOptions options { {}, number_of_inputs };

    TRY(ChannelMergerNode::validate_options(options));
    return ChannelMergerNode::create(*this, options);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createconstantsource
WebIDL::ExceptionOr<GC::Ref<ConstantSourceNode>> BaseAudioContext::create_constant_source()
{
    return ConstantSourceNode::create(*this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createdelay
WebIDL::ExceptionOr<GC::Ref<DelayNode>> BaseAudioContext::create_delay(double max_delay_time)
{
    DelayOptions options { {}, {}, max_delay_time };

    TRY(DelayNode::validate_options(options));
    return DelayNode::create(*this, options);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createchannelsplitter
WebIDL::ExceptionOr<GC::Ref<ChannelSplitterNode>> BaseAudioContext::create_channel_splitter(WebIDL::UnsignedLong number_of_outputs)
{
    ChannelSplitterOptions options { {}, number_of_outputs };

    TRY(ChannelSplitterNode::validate_options(options));
    return ChannelSplitterNode::create(*this, options);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createoscillator
WebIDL::ExceptionOr<GC::Ref<OscillatorNode>> BaseAudioContext::create_oscillator()
{
    // Factory method for an OscillatorNode.
    return OscillatorNode::create(*this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createdynamicscompressor
WebIDL::ExceptionOr<GC::Ref<DynamicsCompressorNode>> BaseAudioContext::create_dynamics_compressor()
{
    // Factory method for a DynamicsCompressorNode.
    return DynamicsCompressorNode::create(*this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-creategain
WebIDL::ExceptionOr<GC::Ref<GainNode>> BaseAudioContext::create_gain()
{
    // Factory method for GainNode.
    return GainNode::create(*this);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createpanner
WebIDL::ExceptionOr<GC::Ref<PannerNode>> BaseAudioContext::create_panner()
{
    // Factory method for a PannerNode.
    return PannerNode::create(*this);
}

WebIDL::ExceptionOr<GC::Ref<PeriodicWave>> BaseAudioContext::create_periodic_wave(Vector<float> const& real, Vector<float> const& imag, PeriodicWaveConstraints const& constraints)
{
    PeriodicWaveOptions options;
    options.real = real;
    options.imag = imag;
    options.disable_normalization = constraints.disable_normalization;

    return PeriodicWave::create_for_constructor(*this, options);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-decodeaudiodata
WebIDL::ExceptionOr<void> BaseAudioContext::decode_audio_data(GC::Ref<WebIDL::Promise> promise, ByteBuffer audio_data, GC::Ptr<WebIDL::CallbackType> success_callback, GC::Ptr<WebIDL::CallbackType> error_callback)
{
    // FIXME: When decodeAudioData is called, the following steps MUST be performed on the control thread:

    // 1. If this's relevant global object's associated Document is not fully active then return a
    //    promise rejected with "InvalidStateError" DOMException.
    auto const& associated_document = relevant_window().associated_document();
    if (!associated_document.is_fully_active())
        return WebIDL::InvalidStateError::create("The document is not fully active."_utf16);

    // 3. Append promise to [[pending promises]] and queue a decoding operation.
    m_pending_promises.append(promise);
    queue_a_decoding_operation(promise, move(audio_data), success_callback, error_callback);

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-decodeaudiodata
void BaseAudioContext::queue_a_decoding_operation(GC::Ref<JS::PromiseCapability> promise, ByteBuffer audio_data, GC::Ptr<WebIDL::CallbackType> success_callback, GC::Ptr<WebIDL::CallbackType> error_callback)
{
    // FIXME: When queuing a decoding operation to be performed on another thread, the following steps
    //        MUST happen on a thread that is not the control thread nor the rendering thread, called
    //        the decoding thread.

    // Decode through LibMedia. Promise settlement and callbacks are posted to the media element task source below.
    if (audio_data.is_empty()) {
        queue_a_media_element_task(GC::create_function(GC::Heap::the(), [this, promise, error_callback] {
            auto& realm = WebIDL::promise_realm(promise);
            HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
            auto error = WebIDL::EncodingError::create("Unable to decode."_utf16);
            WebIDL::reject_promise(promise, error);
            m_pending_promises.remove_first_matching([&promise](auto& pending_promise) { return pending_promise == promise; });
            if (error_callback)
                invoke_error_callback(realm, *error_callback, error);
        }));
        return;
    }

    auto stream = Media::IncrementallyPopulatedStream::create_from_buffer(audio_data);
    auto decoded_result = Media::decode_entire_audio_stream(stream, Optional<u32> { static_cast<u32>(sample_rate()) });

    auto queue_decode_error = [this, promise, error_callback] {
        queue_a_media_element_task(GC::create_function(GC::Heap::the(), [this, promise, error_callback] {
            auto& realm = WebIDL::promise_realm(promise);
            HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
            auto error = WebIDL::EncodingError::create("Unable to decode."_utf16);
            WebIDL::reject_promise(promise, error);
            m_pending_promises.remove_first_matching([&promise](auto& pending_promise) { return pending_promise == promise; });
            if (error_callback)
                invoke_error_callback(realm, *error_callback, error);
        }));
    };

    if (decoded_result.is_error()) {
        queue_decode_error();
        return;
    }

    auto decoded = decoded_result.release_value();
    if (decoded.channels.is_empty() || decoded.channels.first().is_empty() || decoded.channels.size() > NumericLimits<WebIDL::UnsignedLong>::max() || decoded.channels.first().size() > NumericLimits<WebIDL::UnsignedLong>::max()) {
        queue_decode_error();
        return;
    }

    auto buffer = AudioBuffer::create(static_cast<WebIDL::UnsignedLong>(decoded.channels.size()), static_cast<WebIDL::UnsignedLong>(decoded.channels.first().size()), decoded.sample_specification.sample_rate());
    if (buffer.is_error()) {
        queue_decode_error();
        return;
    }

    auto decoded_buffer = buffer.release_value();
    for (size_t channel = 0; channel < decoded.channels.size(); ++channel) {
        auto channel_data = MUST(decoded_buffer->channel_data(channel));
        ReadonlyBytes { reinterpret_cast<u8 const*>(decoded.channels[channel].data()), decoded.channels[channel].size() * sizeof(float) }.copy_to(channel_data->bytes());
    }

    queue_a_media_element_task(GC::create_function(GC::Heap::the(), [this, promise, decoded_buffer, success_callback] {
        auto& promise_realm = WebIDL::promise_realm(promise);
        HTML::TemporaryExecutionContext context(promise_realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
        resolve_audio_buffer_promise(promise_realm, promise, decoded_buffer);
        m_pending_promises.remove_first_matching([&promise](auto& pending_promise) { return pending_promise == promise; });
        if (success_callback)
            invoke_audio_buffer_callback(promise_realm, *success_callback, decoded_buffer);
    }));
    return;
}

WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> BaseAudioContext::decode_audio_data(JS::Realm& realm, GC::Ref<JS::ArrayBuffer> audio_data, GC::Ptr<WebIDL::CallbackType> success_callback, GC::Ptr<WebIDL::CallbackType> error_callback)
{
    auto promise = WebIDL::create_promise_for(relevant_global_object());
    if (audio_data->is_detached()) {
        auto error = WebIDL::DataCloneError::create("Audio data is detached."_utf16);
        WebIDL::reject_promise(promise, error);
        if (error_callback) {
            queue_a_media_element_task(GC::create_function(GC::Heap::the(), [&realm, error_callback, error] {
                invoke_error_callback(realm, *error_callback, error);
            }));
        }
        return promise;
    }
    auto bytes = TRY_OR_THROW_OOM(realm.vm(), audio_data->copy_to_byte_buffer());
    TRY(JS::detach_array_buffer(realm.vm(), *audio_data));
    TRY(decode_audio_data(promise, move(bytes), success_callback, error_callback));
    return promise;
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

    TRY(ScriptProcessorNode::validate_options(buffer_size, number_of_input_channels, number_of_output_channels));
    return ScriptProcessorNode::create(*this, buffer_size, number_of_input_channels, number_of_output_channels);
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createstereopanner
WebIDL::ExceptionOr<GC::Ref<StereoPannerNode>> BaseAudioContext::create_stereo_panner()
{
    // Factory method for a StereoPannerNode.
    return StereoPannerNode::create(*this);
}

WebIDL::ExceptionOr<void> BaseAudioContext::verify_audio_options_inside_nominal_range(float sample_rate)
{
    if (sample_rate < MIN_SAMPLE_RATE || sample_rate > MAX_SAMPLE_RATE)
        return WebIDL::NotSupportedError::create("Sample rate is outside of allowed range"_utf16);

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createbuffer
WebIDL::ExceptionOr<void> BaseAudioContext::verify_audio_options_inside_nominal_range(WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate)
{
    // A NotSupportedError exception MUST be thrown if any of the arguments is negative, zero, or outside its nominal range.

    if (number_of_channels == 0)
        return WebIDL::NotSupportedError::create("Number of channels must not be '0'"_utf16);

    if (number_of_channels > MAX_NUMBER_OF_CHANNELS)
        return WebIDL::NotSupportedError::create("Number of channels is greater than allowed range"_utf16);

    if (length == 0)
        return WebIDL::NotSupportedError::create("Length of buffer must be at least 1"_utf16);

    TRY(verify_audio_options_inside_nominal_range(sample_rate));

    return {};
}

void BaseAudioContext::queue_a_media_element_task(GC::Ref<GC::Function<void()>> steps)
{
    // Rendering completion callbacks run from the audio thread, outside any
    // JavaScript execution context. Resolve the task target from this context's
    // associated global rather than consulting the thread-local current ESO.
    auto task = HTML::Task::create(m_media_element_event_task_source.source, relevant_settings_object().responsible_document(), steps);
    (void)HTML::main_thread_event_loop().task_queue().add(task);
}

void BaseAudioContext::queue_control_message(ControlMessage message)
{
    m_control_message_queue->enqueue(move(message));
    // FIXME: Should signal the rendering thread when implemented
}

}
