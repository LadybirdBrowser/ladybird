/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/ByteBuffer.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Notifier.h>
#include <LibThreading/Mutex.h>
#include <LibWebAudio/GraphNodes/GraphNodeTypes.h>
#include <LibWebAudio/LibWebAudio.h>
#include <LibWebAudio/Script/ScriptProcessorStreamBindings.h>
#include <LibWebAudio/Script/WorkletModule.h>
#include <LibWebAudio/Script/WorkletPortBinding.h>
#include <LibWebAudio/SessionClientOfWebAudioWorker.h>
#include <LibWebAudio/SharedAudioBuffer.h>
#include <LibWebAudio/SharedBufferStream.h>

namespace Web::WebAudio {

class MediaElementAudioSourceProvider;

}

namespace Web::WebAudio::Render {

class GraphResources;

// WebContent-side owner of the WebAudioWorker session and associated shared-memory transports.
//
// This is the only realtime WebAudio execution model: rendering happens out-of-process in
// WebAudioWorker (with AudioServer owning the OS output device).
class SessionRouter : public Weakable<SessionRouter> {
public:
    struct OutputSessionFormat {
        u32 sample_rate { 0 };
        u32 channel_count { 0 };
    };

    explicit SessionRouter(u64 client_id, Atomic<u64>* current_frame, Atomic<u8>* suspended,
        Atomic<u64>* suspend_generation, Atomic<u64>* underrun_frames_total);
    ~SessionRouter();

    // WebAudio server client integration is owned by this layer.
    static void set_webaudio_transport_provider(Function<ErrorOr<IPC::TransportHandle>(u64 page_id)>);

    bool has_session_transport_open() const;
    ErrorOr<void> open_session_transport(u32 target_latency_ms, u64 page_id,
        Function<void(ErrorOr<OutputSessionFormat> const&)> on_complete);
    void close_session_transport();

    u64 session_id() const;

    void update_client_render_graph(f32 graph_sample_rate, ByteBuffer encoded_graph,
        Vector<SharedAudioBufferBinding> shared_audio_buffer_bindings,
        NonnullOwnPtr<GraphResources> resources,
        Vector<WorkletModule> worklet_modules,
        Vector<WorkletNodeDefinition> worklet_node_definitions,
        Vector<WorkletPortBinding> worklet_port_bindings);

    void set_client_suspended(bool suspended, u64 generation);

    bool try_copy_analyser_snapshot(NodeID analyser_node_id, u32 fft_size, Span<f32> out_time_domain,
        Span<f32> out_frequency_db, u64& out_render_quantum_index);

    bool try_copy_dynamics_compressor_reduction(NodeID compressor_node_id, f32& out_reduction_db,
        u64& out_render_quantum_index);

    void refresh_timing();

private:
    ErrorOr<NonnullRefPtr<::Web::WebAudio::SessionClientOfWebAudioWorker>> ensure_webaudio_client(u64 page_id);
    void set_webaudio_session(NonnullRefPtr<::Web::WebAudio::SessionClientOfWebAudioWorker> const& client,
        u64 session_id);
    void clear_webaudio_session();

    void start_time_sync_notifier_if_needed();
    void stop_time_sync_notifier();
    void update_current_frames_from_timing_page();

    bool update_media_element_stream_bindings(GraphResources const& resources,
        Vector<WorkletPortBinding>& worklet_port_bindings);
    void update_media_stream_audio_source_bindings(GraphResources const& resources);

    struct RemoteAnalyserStream {
        u32 fft_size { 0 };
        SharedBufferStream stream;
    };

    struct RemoteDynamicsCompressorStream {
        SharedBufferStream stream;
    };

    struct RemoteMediaElementStream {
        Core::AnonymousBuffer shared_memory;
        RingStreamView view;
        int notify_read_fd { -1 };
        int notify_write_fd { -1 };
        RefPtr<MediaElementAudioSourceProvider> provider;
    };

    struct PendingSuspendState {
        bool suspended { false };
        u64 generation { 0 };
    };

    RefPtr<::Web::WebAudio::SessionClientOfWebAudioWorker> m_webaudio_client;
    u64 m_session_id { 0 };
    u64 m_client_id { 0 };
    Atomic<u64>* m_current_frame { nullptr };
    Atomic<u8>* m_suspended { nullptr };
    Atomic<u64>* m_suspend_generation { nullptr };
    Atomic<u64>* m_underrun_frames_total { nullptr };

    Optional<::Web::WebAudio::SessionClientOfWebAudioWorker::WebAudioSession> m_webaudio_session;

    Core::AnonymousBuffer m_timing_buffer;
    RefPtr<Core::Notifier> m_time_sync_notifier;

    Threading::Mutex m_remote_analyser_streams_mutex;
    HashMap<NodeID, RemoteAnalyserStream> m_remote_analyser_streams;

    Threading::Mutex m_remote_dynamics_compressor_streams_mutex;
    HashMap<NodeID, RemoteDynamicsCompressorStream> m_remote_dynamics_compressor_streams;

    HashMap<MediaElementAudioSourceProviderID, RemoteMediaElementStream> m_remote_media_element_streams;
    HashMap<MediaStreamAudioSourceProviderID, AudioInputStreamMetadata> m_media_stream_source_metadata;

    ScriptProcessorStreamBindings m_script_processor_stream_bindings;
    Optional<PendingSuspendState> m_pending_suspend;

    bool m_published_media_element_stream_bindings { false };
    bool m_published_media_stream_audio_source_bindings { false };
};

} // namespace Web::WebAudio::Render
