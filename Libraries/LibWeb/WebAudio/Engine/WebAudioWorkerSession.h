/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Notifier.h>
#include <LibCore/SharedBufferStream.h>
#include <LibThreading/Mutex.h>
#include <LibWeb/WebAudio/Engine/StreamTransport.h>
#include <LibWeb/WebAudio/Engine/StreamTransportDescriptors.h>
#include <LibWeb/WebAudio/GraphNodes/GraphNodeTypes.h>
#include <LibWeb/WebAudio/ScriptProcessor/ScriptProcessorStreamBindings.h>
#include <LibWeb/WebAudio/Types.h>
#include <LibWeb/WebAudio/Worklet/WorkletModule.h>
#include <LibWeb/WebAudio/Worklet/WorkletNodeDefinition.h>
#include <LibWeb/WebAudio/Worklet/WorkletPortBinding.h>
#include <LibWebAudioWorkerClient/WebAudioClient.h>

namespace Web::WebAudio {

class MediaElementAudioSourceProvider;

}

namespace Web::WebAudio::Render {

class WebAudioClientRegistry;
class GraphResourceRegistry;

// WebContent-side owner of the WebAudioWorker session and associated shared-memory transports.
//
// This is the only realtime WebAudio execution model: rendering happens out-of-process in
// WebAudioWorker (with AudioServer owning the OS output device).
class WebAudioWorkerSession {
public:
    explicit WebAudioWorkerSession(u64 client_id);
    ~WebAudioWorkerSession();

    // WebAudio server client integration is owned by this layer.
    static void set_webaudio_client(NonnullRefPtr<WebAudioWorkerClient::WebAudioClient>);
    static RefPtr<WebAudioWorkerClient::WebAudioClient> webaudio_client();

    bool has_output_open(WebAudioClientRegistry const&) const;
    ErrorOr<void> ensure_output_open(WebAudioClientRegistry&, u32 target_latency_ms, u64 page_id);
    void shutdown_output(WebAudioClientRegistry&);

    u64 session_id() const;

    void update_client_render_graph(
        WebAudioClientRegistry&, u64 client_id, f32 graph_sample_rate, ByteBuffer encoded_graph,
        NonnullOwnPtr<GraphResourceRegistry> resources,
        Vector<WorkletModule> worklet_modules,
        Vector<WorkletNodeDefinition> worklet_node_definitions,
        Vector<WorkletPortBinding> worklet_port_bindings);

    void set_client_suspended(WebAudioClientRegistry&, u64 client_id, bool suspended, u64 generation);

    bool try_copy_analyser_snapshot(
        WebAudioClientRegistry&, u64 client_id, NodeID analyser_node_id, u32 fft_size,
        Span<f32> out_time_domain, Span<f32> out_frequency_db, u64& out_render_quantum_index);

    bool try_copy_dynamics_compressor_reduction(
        WebAudioClientRegistry&, u64 client_id, NodeID compressor_node_id, f32& out_reduction_db, u64& out_render_quantum_index);

private:
    friend class WebAudioClientRegistry;

    void set_webaudio_session(NonnullRefPtr<WebAudioWorkerClient::WebAudioClient> const& client, u64 session_id);
    void clear_webaudio_session();

    void start_time_sync_notifier_if_needed(WebAudioClientRegistry&);
    void stop_time_sync_notifier();
    void update_current_frames_from_timing_page(WebAudioClientRegistry&);

    bool update_media_element_stream_bindings(GraphResourceRegistry const& resources, Vector<WorkletPortBinding>& worklet_port_bindings);
    void update_media_stream_audio_source_bindings(GraphResourceRegistry const& resources);

    struct RemoteAnalyserStream {
        u32 fft_size { 0 };
        Core::SharedBufferStream stream;
    };

    struct RemoteDynamicsCompressorStream {
        Core::SharedBufferStream stream;
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

    RefPtr<WebAudioWorkerClient::WebAudioClient> m_client;
    u64 m_session_id { 0 };
    u64 m_client_id { 0 };

    Optional<WebAudioWorkerClient::WebAudioClient::WebAudioSession> m_webaudio_session;

    Core::AnonymousBuffer m_timing_buffer;
    RefPtr<Core::Notifier> m_time_sync_notifier;

    Threading::Mutex m_remote_analyser_streams_mutex;
    HashMap<NodeID, RemoteAnalyserStream> m_remote_analyser_streams;

    Threading::Mutex m_remote_dynamics_compressor_streams_mutex;
    HashMap<NodeID, RemoteDynamicsCompressorStream> m_remote_dynamics_compressor_streams;

    HashMap<MediaElementAudioSourceProviderID, RemoteMediaElementStream> m_remote_media_element_streams;
    HashMap<MediaStreamAudioSourceProviderID, AudioInputStreamMetadata> m_media_stream_source_metadata;

    ScriptProcessorStreamBindings m_script_processor_stream_bindings;
    Optional<PendingSuspendState> m_pending_suspend_state;

    bool m_published_media_element_stream_bindings { false };
    bool m_published_media_stream_audio_source_bindings { false };
};

}
