/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/File.h>
#include <LibWeb/WebAudio/AudioParamDescriptor.h>
#include <LibWeb/WebAudio/Worklet/WorkletNodeDefinition.h>
#include <LibWebAudio/LibWebAudio.h>
#include <LibWebAudio/SharedAudioBuffer.h>
#include <LibWebAudio/SharedBufferStream.h>
#include <LibWebAudio/ToSessionClientFromWebAudioWorkerEndpoint.h>
#include <LibWebAudio/ToWebAudioWorkerFromSessionClientEndpoint.h>

namespace Web::WebAudio {

class SessionClientOfWebAudioWorker final
    : public RefCounted<SessionClientOfWebAudioWorker>
    , public Weakable<SessionClientOfWebAudioWorker> {
public:
    struct WebAudioSession {
        u64 session_id { 0 };
        u32 sample_rate { 0 };
        u32 channel_count { 0 };
        Core::AnonymousBuffer timing_buffer;
        IPC::File timing_notify_fd;
    };

    static NonnullRefPtr<SessionClientOfWebAudioWorker> create();

    // Install a callback that can obtain a connected socket to the WebAudio server endpoint.
    // In production, this typically asks the broker process to mint a new connection.
    void set_socket_provider(Function<ErrorOr<IPC::File>(u64 page_id)>);

    ErrorOr<void> create_session_async(u32 target_latency_ms, u64 page_id, Function<void(ErrorOr<WebAudioSession>&&)> on_complete);
    ErrorOr<void> destroy_session(u64 session_id);

    ErrorOr<void> add_worklet_module(u64 session_id, u64 module_id, ByteString url, ByteString source_text);
    ErrorOr<void> set_render_graph(u64 session_id, ByteBuffer encoded_graph, Vector<Web::WebAudio::Render::SharedAudioBufferBinding> shared_audio_buffers);
    ErrorOr<void> set_suspended(u64 session_id, bool suspended, u64 generation);
    ErrorOr<void> set_media_element_audio_source_streams(u64 session_id, Vector<Web::WebAudio::Render::MediaElementAudioSourceStreamDescriptor> streams);
    ErrorOr<void> set_media_stream_audio_source_streams(u64 session_id, Vector<Web::WebAudio::Render::MediaStreamAudioSourceStreamDescriptor> streams);
    ErrorOr<void> set_script_processor_streams(u64 session_id, Vector<Web::WebAudio::Render::ScriptProcessorStreamDescriptor> streams);
    ErrorOr<void> set_worklet_node_ports(u64 session_id, Vector<Web::WebAudio::Render::WorkletNodePortDescriptor> ports);
    ErrorOr<void> set_worklet_node_definitions(u64 session_id, Vector<Web::WebAudio::Render::WorkletNodeDefinition> definitions);

    ErrorOr<SharedBufferStream> create_analyser_stream(u64 session_id, u64 analyser_node_id, u32 fft_size, u32 block_count);
    ErrorOr<SharedBufferStream> create_dynamics_compressor_stream(u64 session_id, u64 compressor_node_id, u32 block_count);

    Function<void()> on_death;
    Function<void(u64 session_id, u64 node_id)> on_worklet_processor_error;
    Function<void(u64 session_id, String const& name, Vector<Web::WebAudio::AudioParamDescriptor> const&, u64 generation)> on_worklet_processor_registered;
    Function<void(u64 session_id, u64 module_id, u64 required_generation, bool success, String const& error_name, String const& error_message, Vector<String> const& failed_processor_registrations)> on_worklet_module_evaluated;

private:
    SessionClientOfWebAudioWorker() = default;

    class WebAudioSessionConnection final
        : public IPC::ConnectionToServer<ToSessionClientFromWebAudioWorkerEndpoint, ToWebAudioWorkerFromSessionClientEndpoint>
        , public ToSessionClientFromWebAudioWorkerEndpoint {
        C_OBJECT_ABSTRACT(WebAudioSessionConnection);

    public:
        explicit WebAudioSessionConnection(SessionClientOfWebAudioWorker&, NonnullOwnPtr<IPC::Transport>);

        Function<void()> on_death;

    private:
        void die() override { shutdown(); }
        void shutdown();
        void session_created(u64 request_id, u64 session_id, u32 sample_rate, u32 channel_count, Core::AnonymousBuffer timing_buffer, IPC::File timing_notify_fd) override;
        void session_failed(u64 request_id, ByteString error_message) override;
        void worklet_processor_error(u64 session_id, u64 node_id) override;
        void worklet_processor_registered(u64 session_id, String name, Vector<Web::WebAudio::AudioParamDescriptor> descriptors, u64 generation) override;
        void worklet_module_evaluated(u64 session_id, u64 module_id, u64 required_generation, bool success, String error_name, String error_message, Vector<String> failed_processor_registrations) override;

        SessionClientOfWebAudioWorker& m_client;
    };

    ErrorOr<void> ensure_connection(u64 page_id);
    void maybe_release_idle_connection();
    void handle_session_created(u64 request_id, u64 session_id, u32 sample_rate, u32 channel_count, Core::AnonymousBuffer timing_buffer, IPC::File timing_notify_fd);
    void handle_session_failed(u64 request_id, ByteString const& error_message);
    void fail_pending_create_session_requests(ByteString const& error_message);

    struct PendingCreateSessionRequest {
        Function<void(WebAudioSession&&)> on_success;
        Function<void(ByteString const&)> on_error;
    };

    RefPtr<WebAudioSessionConnection> m_connection;
    Function<ErrorOr<IPC::File>(u64 page_id)> m_socket_provider;
    u64 m_next_create_session_request_id { 1 };
    HashMap<u64, PendingCreateSessionRequest> m_pending_create_session_requests;
    HashTable<u64> m_active_session_ids;
};

}
