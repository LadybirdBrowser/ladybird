/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/BaseAudioContextPrototype.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/WebAudio/AnalyserNode.h>
#include <LibWeb/WebAudio/AudioListener.h>
#include <LibWeb/WebAudio/BiquadFilterNode.h>
#include <LibWeb/WebAudio/ChannelMergerNode.h>
#include <LibWeb/WebAudio/ChannelSplitterNode.h>
#include <LibWeb/WebAudio/ConstantSourceNode.h>
#include <LibWeb/WebAudio/DelayNode.h>
#include <LibWeb/WebAudio/PeriodicWave.h>
#include <LibWeb/WebAudio/ScriptProcessorNode.h>
#include <LibWeb/WebAudio/StereoPannerNode.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebAudio {

class AudioDestinationNode;

// https://webaudio.github.io/web-audio-api/#BaseAudioContext
class BaseAudioContext : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(BaseAudioContext, DOM::EventTarget);

public:
    virtual ~BaseAudioContext() override;

    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createbuffer-numberofchannels
    // > An implementation MUST support at least 32 channels.
    // Other browsers appear to only allow 32 channels - so let's limit ourselves to that too.
    static constexpr WebIDL::UnsignedLong MAX_NUMBER_OF_CHANNELS { 32 };

    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-createbuffer-samplerate
    // > An implementation MUST support sample rates in at least the range 8000 to 96000.
    // This doesn't seem consistent between browsers. We use what firefox accepts from testing BaseAudioContext.createAudioBuffer.
    static constexpr float MIN_SAMPLE_RATE { 8000 };
    static constexpr float MAX_SAMPLE_RATE { 192000 };

    static WebIDL::UnsignedLong render_quantum_size() { return s_render_quantum_size; }

    GC::Ref<AudioDestinationNode> destination() const { return *m_destination; }
    float sample_rate() const { return m_sample_rate; }
    double current_time() const { return m_current_time; }
    GC::Ref<AudioListener> listener() const { return m_listener; }
    Bindings::AudioContextState state() const { return m_control_thread_state; }

    // https://webaudio.github.io/web-audio-api/#--nyquist-frequency
    float nyquist_frequency() const { return m_sample_rate / 2; }

    void set_onstatechange(WebIDL::CallbackType*);
    WebIDL::CallbackType* onstatechange();

    void set_sample_rate(float sample_rate) { m_sample_rate = sample_rate; }
    void set_control_state(Bindings::AudioContextState state) { m_control_thread_state = state; }
    void set_rendering_state(Bindings::AudioContextState state) { m_rendering_thread_state = state; }

    static WebIDL::ExceptionOr<void> verify_audio_options_inside_nominal_range(JS::Realm&, float sample_rate);
    static WebIDL::ExceptionOr<void> verify_audio_options_inside_nominal_range(JS::Realm&, WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate);

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
    WebIDL::ExceptionOr<GC::Ref<PeriodicWave>> create_periodic_wave(Vector<float> const& real, Vector<float> const& imag, Optional<PeriodicWaveConstraints> const& constraints = {});
    WebIDL::ExceptionOr<GC::Ref<ScriptProcessorNode>> create_script_processor(WebIDL::UnsignedLong buffer_size,
        WebIDL::UnsignedLong number_of_input_channels, WebIDL::UnsignedLong number_of_output_channels);
    WebIDL::ExceptionOr<GC::Ref<StereoPannerNode>> create_stereo_panner();

    GC::Ref<WebIDL::Promise> decode_audio_data(GC::Root<WebIDL::BufferSource>, GC::Ptr<WebIDL::CallbackType>, GC::Ptr<WebIDL::CallbackType>);

protected:
    explicit BaseAudioContext(JS::Realm&, float m_sample_rate = 0);

    void queue_a_media_element_task(GC::Ref<GC::Function<void()>>);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<AudioDestinationNode> m_destination;
    Vector<GC::Ref<WebIDL::Promise>> m_pending_promises;

private:
    // https://webaudio.github.io/web-audio-api/#render-quantum-size
    static constexpr WebIDL::UnsignedLong s_render_quantum_size { 128 };

    void queue_a_decoding_operation(GC::Ref<JS::PromiseCapability>, GC::Root<WebIDL::BufferSource> const&, GC::Ptr<WebIDL::CallbackType>, GC::Ptr<WebIDL::CallbackType>);

    float m_sample_rate { 0 };
    double m_current_time { 0 };

    GC::Ref<AudioListener> m_listener;

    Bindings::AudioContextState m_control_thread_state = Bindings::AudioContextState::Suspended;
    Bindings::AudioContextState m_rendering_thread_state = Bindings::AudioContextState::Suspended;

    HTML::UniqueTaskSource m_media_element_event_task_source {};
};

}
