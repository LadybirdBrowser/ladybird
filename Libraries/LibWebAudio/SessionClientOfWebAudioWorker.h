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
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/File.h>
#include <LibWebAudio/LibWebAudio.h>
#include <LibWebAudio/SharedAudioBuffer.h>
#include <LibWebAudio/SharedBufferStream.h>
#include <LibWebAudio/ToSessionClientFromWebAudioWorkerEndpoint.h>
#include <LibWebAudio/ToWebAudioWorkerFromSessionClientEndpoint.h>

namespace Web::WebAudio {

class SessionClientOfWebAudioWorker final
    : public IPC::ConnectionToServer<ToSessionClientFromWebAudioWorkerEndpoint,
          ToWebAudioWorkerFromSessionClientEndpoint>
    , public ToSessionClientFromWebAudioWorkerEndpoint {
    C_OBJECT_ABSTRACT(SessionClientOfWebAudioWorker);

public:
    struct WebAudioSession {
        u64 session_id { 0 };
        u32 sample_rate { 0 };
        u32 channel_count { 0 };
        Core::AnonymousBuffer timing_buffer;
        IPC::File timing_notify_fd;
    };

    static NonnullRefPtr<SessionClientOfWebAudioWorker> create(NonnullOwnPtr<IPC::Transport>);

    ErrorOr<void> create_session_async(u32 target_latency_ms,
        Function<void(ErrorOr<WebAudioSession>&&)> on_complete);
    ErrorOr<void> destroy_session(u64 session_id);

    ErrorOr<void> add_worklet_module(u64 session_id, u64 module_id, ByteString url, ByteString source_text);
    ErrorOr<void> set_render_graph(u64 session_id, ByteBuffer encoded_graph,
        Vector<Render::SharedAudioBufferBinding> shared_audio_buffers);
    ErrorOr<void> set_suspended(u64 session_id, bool suspended, u64 generation);
    ErrorOr<void>
    set_media_element_audio_source_streams(u64 session_id,
        Vector<Render::MediaElementAudioSourceStreamDescriptor> streams);
    ErrorOr<void>
    set_media_stream_audio_source_streams(u64 session_id,
        Vector<Render::MediaStreamAudioSourceStreamDescriptor> streams);
    ErrorOr<void> set_script_processor_streams(u64 session_id,
        Vector<Render::ScriptProcessorStreamDescriptor> streams);
    ErrorOr<void> set_worklet_node_ports(u64 session_id, Vector<Render::WorkletNodePortDescriptor> ports);
    ErrorOr<void> set_worklet_node_definitions(u64 session_id,
        Vector<Render::WorkletNodeDefinition> definitions);

    ErrorOr<SharedBufferStream> create_analyser_stream(u64 session_id, u64 analyser_node_id, u32 fft_size,
        u32 block_count);
    ErrorOr<SharedBufferStream> create_dynamics_compressor_stream(u64 session_id, u64 compressor_node_id,
        u32 block_count);

    Function<void()> on_death;
    Function<void(u64 session_id, u64 node_id)> on_worklet_processor_error;
    Function<void(u64 session_id, String const& name, Vector<Render::AudioParamDescriptor> const&,
        u64 generation)>
        on_worklet_processor_registered;
    Function<void(u64 session_id, u64 module_id, u64 required_generation, bool success,
        String const& error_name, String const& error_message,
        Vector<String> const& failed_processor_registrations)>
        on_worklet_module_evaluated;

private:
    explicit SessionClientOfWebAudioWorker(NonnullOwnPtr<IPC::Transport>);

    void die() override { shutdown(); }
    void shutdown();
    void session_created(u64 request_id, u64 session_id, u32 sample_rate, u32 channel_count,
        Core::AnonymousBuffer timing_buffer, IPC::File timing_notify_fd) override;
    void session_failed(u64 request_id, ByteString error_message) override;
    void worklet_processor_error(u64 session_id, u64 node_id) override;
    void worklet_processor_registered(u64 session_id, String name,
        Vector<Render::AudioParamDescriptor> descriptors,
        u64 generation) override;
    void worklet_module_evaluated(u64 session_id, u64 module_id, u64 required_generation, bool success,
        String error_name, String error_message,
        Vector<String> failed_processor_registrations) override;

    void fail_pending_create_session_requests();

    struct PendingCreateSessionRequest {
        Function<void(ErrorOr<WebAudioSession>&&)> on_complete;
    };

    u64 m_next_create_session_request_id { 1 };
    HashMap<u64, PendingCreateSessionRequest> m_pending_create_session_requests;
};

} // namespace Web::WebAudio
