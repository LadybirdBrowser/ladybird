/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/SPSCQueue.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <AudioServer/AudioInputDeviceInfo.h>
#include <AudioServer/AudioInputStreamDescriptor.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/SharedBufferStream.h>
#include <LibCore/SharedSingleProducerCircularBuffer.h>
#include <LibThreading/Forward.h>
#include <LibThreading/Mutex.h>
#include <LibWeb/WebAudio/AudioParamDescriptor.h>
#include <LibWeb/WebAudio/Engine/GraphCodec.h>
#include <LibWeb/WebAudio/Engine/SharedMemory.h>
#include <LibWeb/WebAudio/Engine/SincResampler.h>
#include <LibWeb/WebAudio/Engine/StreamTransportDescriptors.h>
#include <LibWeb/WebAudio/MediaElementAudioSourceProvider.h>
#include <LibWeb/WebAudio/Worklet/WorkletModule.h>
#include <LibWeb/WebAudio/Worklet/WorkletNodeDefinition.h>
#include <LibWeb/WebAudio/Worklet/WorkletPortBinding.h>

namespace Core {

class WeakEventLoopReference;

}

namespace Web::WebAudio::Render {

struct GraphDescription;
struct WireGraphBuildResult;
class AudioBus;
class GraphExecutor;
class RealtimeAudioWorkletProcessorHost;

}

namespace WebAudioWorker {

using WireGraphBuildResult = Web::WebAudio::Render::WireGraphBuildResult;

class SessionScriptProcessorHost;

struct RenderState {
    // Render-loop scratch buffers (render-thread owned). Preallocated at session setup time.
    Vector<float> interleaved;
    Vector<ReadonlySpan<f32>> planar_spans;
    OwnPtr<Web::WebAudio::Render::AudioBus> mix_bus;
    OwnPtr<Web::WebAudio::Render::AudioBus> context_mix_bus;

    size_t bytes_per_frame { 0 };

    // Rendered frames in the WebAudio context timeline (i.e. at context sample rate).
    u64 rendered_frames { 0 };

    // Frames written to the output ring (i.e. at output device sample rate).
    u64 frames_written { 0 };

    // Output resampling state, used when graph runs at a different rate than the output device.
    bool resampler_initialized { false };
    u32 resampler_last_context_sample_rate { 0 };
    u32 resampler_last_device_sample_rate { 0 };
    size_t resampler_last_channel_count { 0 };
    Web::WebAudio::Render::SampleRateConverter resampler;
    // Resampler input staging (context sample rate -> device sample rate).
    // This is a fixed-capacity ring buffer per channel, to keep render-thread work bounded.
    Vector<Vector<f32>> resample_input_channels;
    Vector<Vector<f32>> resample_input_scratch_channels;
    Vector<ReadonlySpan<f32>> resample_input_spans;
    Vector<Span<f32>> resample_output_spans;
    size_t resample_input_read_index { 0 };
    size_t resample_input_available_frames { 0 };

    Atomic<u64> underrun_frames { 0 };
};

struct WorkletState {
    Vector<Web::WebAudio::Render::WorkletModule> modules;

    struct ProcessorRegistration {
        String name;
        Vector<Web::WebAudio::AudioParamDescriptor> descriptors;
        u64 generation { 0 };
    };

    struct ModuleEvaluation {
        u64 module_id { 0 };
        u64 required_generation { 0 };
        bool success { false };
        String error_name;
        String error_message;
        Vector<String> failed_processor_registrations;
    };

    Threading::Mutex definitions_mutex;
    Vector<Web::WebAudio::Render::WorkletNodeDefinition> node_definitions;

    Threading::Mutex host_mutex;
    OwnPtr<Web::WebAudio::Render::RealtimeAudioWorkletProcessorHost> host;
    Atomic<Web::WebAudio::Render::RealtimeAudioWorkletProcessorHost*> host_ptr { nullptr };
    Function<void(Web::WebAudio::NodeID)> processor_error_callback;
    Function<void(String const&, Vector<Web::WebAudio::AudioParamDescriptor> const&, u64)> processor_registration_callback;
    Function<void(u64 module_id, u64 required_generation, bool success, String const& error_name, String const& error_message, Vector<String> failed_processor_registrations)> module_evaluation_callback;

    Threading::Mutex ports_mutex;
    HashMap<u64, int> processor_port_fds;

    RefPtr<Core::WeakEventLoopReference> control_event_loop;
    AK::SPSCQueue<Web::WebAudio::NodeID, 1024> error_queue;
    Atomic<bool> error_task_scheduled { false };
    AK::SPSCQueue<ProcessorRegistration, 256> registration_queue;
    Atomic<bool> registration_task_scheduled { false };
    AK::SPSCQueue<ModuleEvaluation, 256> module_evaluation_queue;
    Atomic<bool> module_evaluation_task_scheduled { false };
};

struct StreamState {
    struct Analyser {
        u32 fft_size { 0 };
        Core::SharedBufferStream stream;
    };
    struct DynamicsCompressor {
        Core::SharedBufferStream stream;
    };
    struct MediaElement {
        NonnullRefPtr<Web::WebAudio::MediaElementAudioSourceProvider> provider;
    };
    struct MediaStream {
        Web::WebAudio::Render::AudioInputStreamMetadata metadata;
        AudioServer::AudioInputStreamID stream_id { 0 };
        NonnullRefPtr<Web::WebAudio::MediaElementAudioSourceProvider> provider;
    };
    struct ScriptProcessorStreamMap : public AtomicRefCounted<ScriptProcessorStreamMap> {
    public:
        struct StreamState {
            Web::WebAudio::Render::ScriptProcessorStreamDescriptor descriptor;
            Core::SharedBufferStream request_stream;
            Core::SharedBufferStream response_stream;
        };

        HashMap<u64, StreamState> streams;
    };
    struct AnalyserStreamMap;
    struct DynamicsCompressorStreamMap;

    Threading::Mutex analyser_streams_mutex;
    Threading::Mutex dynamics_compressor_streams_mutex;
    Threading::Mutex media_element_streams_mutex;
    Threading::Mutex media_stream_streams_mutex;
    Threading::Mutex script_processor_streams_mutex;

    Atomic<AnalyserStreamMap*> analyser_streams { nullptr };
    Atomic<DynamicsCompressorStreamMap*> dynamics_compressor_streams { nullptr };
    HashMap<u64, MediaElement> media_element_streams;
    HashMap<u64, MediaStream> media_stream_streams;
    Atomic<ScriptProcessorStreamMap*> script_processor_streams { nullptr };
};

class WebAudioSession
    : public RefCounted<WebAudioSession>
    , public Weakable<WebAudioSession> {
public:
    WebAudioSession(u64 session_id, u32 device_sample_rate_hz, u32 device_channel_count, Core::AnonymousBuffer timing_buffer, int timing_notify_write_fd, int client_id);
    ~WebAudioSession();

    u64 session_id() const { return m_session_id; }
    u32 device_sample_rate_hz() const { return m_device_sample_rate_hz; }
    u32 device_channel_count() const { return m_device_channel_count; }

    void set_render_graph(WireGraphBuildResult graph);

    void set_analyser_stream(u64 analyser_node_id, u32 fft_size, Core::SharedBufferStream);
    void set_dynamics_compressor_stream(u64 compressor_node_id, Core::SharedBufferStream);
    void set_media_element_audio_source_streams(Vector<Web::WebAudio::Render::MediaElementAudioSourceStreamDescriptor>&&);
    void set_media_stream_audio_source_streams(Vector<Web::WebAudio::Render::MediaStreamAudioSourceStreamDescriptor> const&);
    void set_script_processor_streams(Vector<Web::WebAudio::Render::ScriptProcessorStreamDescriptor>);

    void add_worklet_module(u64 module_id, ByteString url, ByteString source_text);
    void set_worklet_node_ports(Vector<Web::WebAudio::Render::WorkletNodePortDescriptor> const&);
    void set_worklet_node_definitions(Vector<Web::WebAudio::Render::WorkletNodeDefinition> const&);
    void set_worklet_processor_error_callback(Function<void(Web::WebAudio::NodeID)>);
    void set_worklet_processor_registration_callback(Function<void(String const&, Vector<Web::WebAudio::AudioParamDescriptor> const&, u64)>);
    void set_worklet_module_evaluation_callback(Function<void(u64 module_id, u64 required_generation, bool success, String const& error_name, String const& error_message, Vector<String> failed_processor_registrations)>);

    AudioServer::AudioInputStreamID create_audio_input_stream(AudioServer::AudioInputDeviceID device_id, u32 sample_rate_hz, u32 channel_count, u64 capacity_frames, u8 overflow_policy);
    void destroy_audio_input_stream(AudioServer::AudioInputStreamID stream_id);

    void set_suspended(bool suspended, u64 generation);

    bool render_one_quantum();
    ReadonlySpan<f32> interleaved_output() const;

private:
    friend class SessionScriptProcessorHost;

    struct PreparedGraph;
    struct ThreadLoopState {
        u64 last_level_log_frame { 0 };
        f64 level_sum_squares { 0.0 };
        u64 level_sample_count { 0 };
        f32 level_peak { 0.0f };

        RefPtr<PreparedGraph> current_graph;
        u64 last_seen_generation { 0 };

        bool current_graph_has_script_processor { false };
        bool current_graph_has_media_element_source { false };
        bool current_graph_has_worklet_render_nodes { false };
        u64 graph_swap_output_frame { 0 };
        bool logged_script_processor_never_ran_for_graph { false };

        bool last_timing_page_was_suspended { false };
    };

    void shutdown();
    void initialize_render_state();
    void apply_render_graph(WireGraphBuildResult graph);
    void apply_deferred_graph_if_any();
    void retire_graph_on_control_thread(PreparedGraph*);
    void drain_retired_graphs_on_control_thread();
    void ensure_worklet_host();
    void notify_worklet_processor_error_from_render_thread(Web::WebAudio::NodeID);
    void flush_worklet_processor_errors();
    void notify_worklet_processor_registered_from_render_thread(String const& name, Vector<Web::WebAudio::AudioParamDescriptor> const& descriptors, u64 generation);
    void notify_worklet_module_evaluated_from_render_thread(u64 module_id, u64 required_generation, bool success, String const& error_name, String const& error_message, Vector<String> failed_processor_registrations);
    void flush_worklet_processor_registrations();
    void flush_worklet_module_evaluations();
    void ensure_render_thread_scratch_initialized();
    void maybe_swap_graph(ThreadLoopState&, u64 generation);
    void maybe_log_script_processor_never_ran(ThreadLoopState&);
    void prepare_output_buffers(size_t device_channel_count, size_t quantum_frames);
    void service_audio_worklet_host();
    void render_graph_quantum(ThreadLoopState&, bool quantum_is_suspended);
    void publish_analyser_snapshots(ThreadLoopState const&) const;
    void publish_dynamics_compressor_snapshots(ThreadLoopState const&) const;
    void update_timing_page_and_notify(u64 quantum_was_suspended);
    void update_and_maybe_log_output_levels(ThreadLoopState&);

    u64 m_session_id { 0 };
    int m_client_id { -1 };
    u32 m_device_sample_rate_hz { 0 };
    u32 m_device_channel_count { 0 };
    Atomic<u32> m_context_sample_rate_hz { 0 };

    WorkletState m_worklet;
    StreamState m_streams;
    RenderState m_scratch;

    Threading::Mutex m_graph_mutex;
    Atomic<PreparedGraph*> m_pending_graph { nullptr };
    Atomic<PreparedGraph*> m_active_graph { nullptr };
    Optional<WireGraphBuildResult> m_deferred_graph;
    Atomic<u64> m_graph_generation { 0 };

    struct RetiredGraphNode;
    Atomic<RetiredGraphNode*> m_retired_graphs { nullptr };
    Atomic<bool> m_retired_graph_task_scheduled { false };

    Atomic<u64> m_script_processor_processed_blocks { 0 };
    Atomic<u64> m_script_processor_timeout_blocks { 0 };
    OwnPtr<SessionScriptProcessorHost> m_script_processor_host;

    // Requested suspend state from the control process (encoded with encode_webaudio_suspend_state()).
    Atomic<u64> m_requested_suspend_state { 0 };

    Core::AnonymousBuffer m_timing_buffer;
    Web::WebAudio::Render::WebAudioTimingPage* m_timing_page { nullptr };

    int m_timing_notify_write_fd { -1 };

    HashMap<AudioServer::AudioInputStreamID, AudioServer::AudioInputStreamDescriptor> m_audio_input_streams;

    ThreadLoopState m_thread_state;
};

}
