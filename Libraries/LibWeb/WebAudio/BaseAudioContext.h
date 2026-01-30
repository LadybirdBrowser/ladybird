/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Function.h>
#include <AK/Span.h>
#include <LibGC/Weak.h>
#include <LibWeb/Bindings/BaseAudioContextPrototype.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/WebAudio/AnalyserNode.h>
#include <LibWeb/WebAudio/AssociatedTaskQueue.h>
#include <LibWeb/WebAudio/AudioListener.h>
#include <LibWeb/WebAudio/BiquadFilterNode.h>
#include <LibWeb/WebAudio/ChannelMergerNode.h>
#include <LibWeb/WebAudio/ChannelSplitterNode.h>
#include <LibWeb/WebAudio/ConstantSourceNode.h>
#include <LibWeb/WebAudio/ControlMessage.h>
#include <LibWeb/WebAudio/DelayNode.h>
#include <LibWeb/WebAudio/Engine/RenderContext.h>
#include <LibWeb/WebAudio/PeriodicWave.h>
#include <LibWeb/WebAudio/ScriptProcessorNode.h>
#include <LibWeb/WebAudio/StereoPannerNode.h>
#include <LibWeb/WebAudio/Types.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebAudio {

class AudioDestinationNode;
class AudioWorklet;
class AudioNode;
class ControlMessageQueue;
class BackgroundAudioDecoder;

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

    static WebIDL::UnsignedLong render_quantum_size() { return Render::RENDER_QUANTUM_SIZE; }

    NodeID allocate_audio_node_id();

    GC::Ref<AudioDestinationNode> destination() const { return *m_destination; }
    float sample_rate() const { return m_sample_rate; }
    // https://webaudio.github.io/web-audio-api/#dom-baseaudiocontext-currenttime
    // currentTime MUST be read atomically on the control thread.
    // We store [[current frame]] atomically and derive currentTime from it.
    double current_time() const
    {
        auto frame = m_current_frame.load(AK::MemoryOrder::memory_order_acquire);
        if (m_sample_rate <= 0)
            return 0.0;
        return static_cast<double>(frame) / static_cast<double>(m_sample_rate);
    }
    u64 current_frame() const { return m_current_frame.load(AK::MemoryOrder::memory_order_acquire); }
    GC::Ref<AudioListener> listener() const { return m_listener; }
    Bindings::AudioContextState state() const { return m_control_thread_state; }
    GC::Ref<AudioWorklet> audio_worklet() const;

    // Returns analyser data produced by the realtime renderer, regardless of where it executes.
    // out_frequency_db may be empty to request only time-domain data.
    virtual bool try_copy_realtime_analyser_data(NodeID, u32 fft_size, Span<f32> out_time_domain, Span<f32> out_frequency_db, u64& out_render_quantum_index) const
    {
        (void)fft_size;
        (void)out_time_domain;
        (void)out_frequency_db;
        (void)out_render_quantum_index;
        return false;
    }

    virtual bool try_copy_realtime_dynamics_compressor_reduction(NodeID, f32& out_reduction_db, u64& out_render_quantum_index) const
    {
        (void)out_reduction_db;
        (void)out_render_quantum_index;
        return false;
    }

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

    GC::Ref<WebIDL::Promise> decode_audio_data(GC::Root<WebIDL::BufferSource> const&, GC::Ptr<WebIDL::CallbackType>, GC::Ptr<WebIDL::CallbackType>);

    bool take_pending_promise(GC::Ref<WebIDL::Promise> const&);

    void queue_control_message(ControlMessage);

    NodeID next_node_id();

    void queue_associated_task(AssociatedTaskQueue::Task);
    void queue_a_media_element_task(GC::Ref<GC::Function<void()>>);

    void notify_audio_graph_changed();

    // Internal: allow render-graph snapshotting to find nodes that are not reachable from the
    // destination (e.g. AudioWorkletNodes with zero outputs).
    void register_audio_node_for_snapshot(AudioNode&);
    ReadonlySpan<GC::Weak<AudioNode>> audio_nodes_for_snapshot() const { return m_audio_nodes_for_snapshot.span(); }

    void flush_pending_audio_graph_update();

    bool audio_graph_dirty_for_debug() const { return m_audio_graph_dirty; }
    bool audio_graph_update_task_scheduled_for_debug() const { return m_audio_graph_update_task_scheduled; }

protected:
    explicit BaseAudioContext(JS::Realm&, float m_sample_rate = 0);

    ControlMessageQueue& control_message_queue() { return *m_control_message_queue; }
    ControlMessageQueue const& control_message_queue() const { return *m_control_message_queue; }

    AssociatedTaskQueue& associated_task_queue() { return *m_associated_task_queue; }
    AssociatedTaskQueue const& associated_task_queue() const { return *m_associated_task_queue; }

    // Render-thread / derived-context helpers.
    void set_current_frame(u64 frame) { m_current_frame.store(frame, AK::MemoryOrder::memory_order_release); }
    Atomic<u64>& current_frame_atomic() { return m_current_frame; }

    Atomic<u64>& render_thread_suspend_state_atomic() { return m_render_thread_suspend_state; }

    virtual void on_audio_graph_changed() { }

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<AudioDestinationNode> m_destination;
    Vector<GC::Ref<WebIDL::Promise>> m_pending_promises;

private:
    friend class BackgroundAudioDecoder;

    u64 m_next_node_id { 0 };

    float m_sample_rate { 0 };
    Atomic<u64> m_current_frame { 0 };

    // Encoded suspend state published by the rendering backend.
    // See Render::encode_webaudio_suspend_state().
    Atomic<u64> m_render_thread_suspend_state { 0 };

    NodeID m_next_audio_node_id { 1 };

    GC::Ref<AudioListener> m_listener;
    mutable GC::Ptr<AudioWorklet> m_audio_worklet;

    Bindings::AudioContextState m_control_thread_state = Bindings::AudioContextState::Suspended;
    Bindings::AudioContextState m_rendering_thread_state = Bindings::AudioContextState::Suspended;

    HTML::UniqueTaskSource m_media_element_event_task_source;

    // Coalesce multiple connect/disconnect/param mutations that occur back-to-back on the control
    // thread into a single graph snapshot/update per event-loop turn.
    // This avoids committing transient intermediate graph states (e.g. disconnected graphs between
    // a disconnect() and a reconnect()) to the realtime render graph.
    HTML::UniqueTaskSource m_audio_graph_update_task_source;
    bool m_audio_graph_dirty { false };
    bool m_audio_graph_update_task_scheduled { false };

    NonnullOwnPtr<ControlMessageQueue> m_control_message_queue;
    NonnullOwnPtr<AssociatedTaskQueue> m_associated_task_queue;

    Vector<GC::Weak<AudioNode>> m_audio_nodes_for_snapshot;
};

}
