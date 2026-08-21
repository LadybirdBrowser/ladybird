/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Function.h>
#include <LibWeb/Bindings/BaseAudioContext.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/WebAudio/AnalyserNode.h>
#include <LibWeb/WebAudio/AudioListener.h>
#include <LibWeb/WebAudio/BiquadFilterNode.h>
#include <LibWeb/WebAudio/ChannelMergerNode.h>
#include <LibWeb/WebAudio/ChannelSplitterNode.h>
#include <LibWeb/WebAudio/ConstantSourceNode.h>
#include <LibWeb/WebAudio/ControlMessage.h>
#include <LibWeb/WebAudio/ControlMessageQueue.h>
#include <LibWeb/WebAudio/DelayNode.h>
#include <LibWeb/WebAudio/PeriodicWave.h>
#include <LibWeb/WebAudio/ScriptProcessorNode.h>
#include <LibWeb/WebAudio/StereoPannerNode.h>
#include <LibWeb/WebAudio/Types.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebIDL {

class DOMException;

}

namespace Web::WebAudio {

class AudioDestinationNode;

using AudioContextState = Bindings::AudioContextState;

// https://webaudio.github.io/web-audio-api/#BaseAudioContext
class BaseAudioContext : public DOM::EventTarget {
    WEB_WRAPPABLE(BaseAudioContext, DOM::EventTarget);

public:
    static constexpr size_t destination_offset() { return offsetof(BaseAudioContext, m_destination); }
    virtual ~BaseAudioContext() override;

    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createbuffer-numberofchannels
    // > An implementation MUST support at least 32 channels.
    // Other browsers appear to only allow 32 channels - so let's limit ourselves to that too.
    static constexpr WebIDL::UnsignedLong MAX_NUMBER_OF_CHANNELS { 32 };

    // https://webaudio.github.io/web-audio-api/#sample-rates
    // Implementations MUST support sample rates between 3000 Hz and 768000 Hz, inclusive. A NotSupportedError MUST
    // be thrown if a sample rate outside this range is specified.
    static constexpr float MIN_SAMPLE_RATE { 3000 };
    static constexpr float MAX_SAMPLE_RATE { 768000 };

    static WebIDL::UnsignedLong render_quantum_size() { return s_render_quantum_size; }

    GC::Ref<AudioDestinationNode> destination() const { return *m_destination; }
    float sample_rate() const { return m_sample_rate; }
    virtual double current_time() const { return m_current_time; }
    GC::Ref<AudioListener> listener() const
    {
        VERIFY(m_listener);
        return *m_listener;
    }
    AudioContextState state() const { return m_control_thread_state; }
    HTML::EnvironmentSettingsObject& relevant_settings_object() const;
    JS::Object& relevant_global_object() const;
    GC::Ref<DOM::Event> create_associated_event(Utf16FlyString const&) const;
    HTML::Window& relevant_window() const;

    // https://webaudio.github.io/web-audio-api/#--nyquist-frequency
    float nyquist_frequency() const { return m_sample_rate / 2; }

    void set_onstatechange(WebIDL::CallbackType*);
    WebIDL::CallbackType* onstatechange();

    void set_sample_rate(float sample_rate) { m_sample_rate = sample_rate; }
    void set_control_state(AudioContextState state) { m_control_thread_state = state; }
    void set_rendering_state(AudioContextState state) { m_rendering_thread_state = state; }

    static WebIDL::ExceptionOr<void> verify_audio_options_inside_nominal_range(float sample_rate);
    static WebIDL::ExceptionOr<void> verify_audio_options_inside_nominal_range(WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate);

    WebIDL::ExceptionOr<GC::Ref<AnalyserNode>> create_analyser();
    WebIDL::ExceptionOr<GC::Ref<BiquadFilterNode>> create_biquad_filter();
    WebIDL::ExceptionOr<GC::Ref<AudioBuffer>> create_buffer(WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate);
    WebIDL::ExceptionOr<GC::Ref<AudioBufferSourceNode>> create_buffer_source();
    WebIDL::ExceptionOr<GC::Ref<ChannelMergerNode>> create_channel_merger(WebIDL::UnsignedLong number_of_inputs);
    WebIDL::ExceptionOr<GC::Ref<ConstantSourceNode>> create_constant_source();
    WebIDL::ExceptionOr<GC::Ref<ChannelSplitterNode>> create_channel_splitter(WebIDL::UnsignedLong number_of_outputs);
    WebIDL::ExceptionOr<GC::Ref<DelayNode>> create_delay(double max_delay_time = 1);
    WebIDL::ExceptionOr<GC::Ref<OscillatorNode>> create_oscillator();
    WebIDL::ExceptionOr<GC::Ref<DynamicsCompressorNode>> create_dynamics_compressor();
    WebIDL::ExceptionOr<GC::Ref<GainNode>> create_gain();
    WebIDL::ExceptionOr<GC::Ref<PannerNode>> create_panner();
    WebIDL::ExceptionOr<GC::Ref<PeriodicWave>> create_periodic_wave(Vector<float> const& real, Vector<float> const& imag, PeriodicWaveConstraints const& constraints = {});
    WebIDL::ExceptionOr<GC::Ref<ScriptProcessorNode>> create_script_processor(
        WebIDL::UnsignedLong buffer_size,
        WebIDL::UnsignedLong number_of_input_channels,
        WebIDL::UnsignedLong number_of_output_channels);
    WebIDL::ExceptionOr<GC::Ref<StereoPannerNode>> create_stereo_panner();

    WebIDL::ExceptionOr<void> decode_audio_data(GC::Ref<WebIDL::Promise>, ByteBuffer, GC::Ptr<WebIDL::CallbackType>, GC::Ptr<WebIDL::CallbackType>);
    WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> decode_audio_data(JS::Realm&, GC::Ref<JS::ArrayBuffer>, GC::Ptr<WebIDL::CallbackType>, GC::Ptr<WebIDL::CallbackType>);

    void queue_control_message(ControlMessage);
    NonnullRefPtr<ControlMessageQueue> control_message_queue() const { return m_control_message_queue; }

    void add_playing_source(GC::Ref<AudioNode>);
    void handle_ended_sources(ReadonlySpan<NodeID>);

    void set_current_time(double current_time) { m_current_time = current_time; }

    NodeID next_node_id(Badge<AudioNode>) { return ++m_next_node_id; }

protected:
    explicit BaseAudioContext(GC::Ref<DOM::EventTarget> relevant_global_object, float m_sample_rate = 0);

    // Called when the associated document is destroyed or otherwise stops being fully active, so rendering resources
    // can be released.
    virtual void document_became_inactive() { }

    void queue_a_media_element_task(GC::Ref<GC::Function<void()>>);
    void set_listener(GC::Ref<AudioListener> listener) { m_listener = listener; }
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<AudioDestinationNode> m_destination;
    Vector<GC::Ref<WebIDL::Promise>> m_pending_promises;

private:
    // https://webaudio.github.io/web-audio-api/#render-quantum-size
    static constexpr WebIDL::UnsignedLong s_render_quantum_size { 128 };

    void queue_a_decoding_operation(GC::Ref<JS::PromiseCapability>, ByteBuffer, GC::Ptr<WebIDL::CallbackType>, GC::Ptr<WebIDL::CallbackType>);

    u64 m_next_node_id { 0 };

    float m_sample_rate { 0 };
    double m_current_time { 0 };

    GC::Ptr<AudioListener> m_listener;
    GC::Ref<DOM::EventTarget> m_global_object;

    AudioContextState m_control_thread_state = AudioContextState::Suspended;
    AudioContextState m_rendering_thread_state = AudioContextState::Suspended;

    HTML::UniqueTaskSource m_media_element_event_task_source {};

    GC::Ptr<DOM::DocumentObserver> m_document_observer;

    NonnullRefPtr<ControlMessageQueue> m_control_message_queue;

    HashMap<NodeID, GC::Ref<AudioNode>> m_playing_sources;
};

void resolve_audio_buffer_promise(JS::Realm&, WebIDL::Promise const&, AudioBuffer&);

}
