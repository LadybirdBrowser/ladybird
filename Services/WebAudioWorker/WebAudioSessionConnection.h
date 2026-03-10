/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibAudioServer/LibAudioServer.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibWeb/WebAudio/Worklet/WorkletNodeDefinition.h>
#include <LibWebAudio/LibWebAudio.h>
#include <LibWebAudio/ToSessionClientFromWebAudioWorkerEndpoint.h>
#include <LibWebAudio/ToWebAudioWorkerFromSessionClientEndpoint.h>
#include <WebAudioWorker/WebAudioRenderThread.h>
#include <WebAudioWorker/WebAudioSession.h>

namespace Web::WebAudio {

class WebAudioSessionConnection final
    : public IPC::ConnectionFromClient<ToSessionClientFromWebAudioWorkerEndpoint, ToWebAudioWorkerFromSessionClientEndpoint> {
    C_OBJECT(WebAudioSessionConnection);

public:
    WebAudioSessionConnection(NonnullOwnPtr<IPC::Transport>, int owner_client_id);
    ~WebAudioSessionConnection() override;

    void die() override { shutdown(); }
    void shutdown();

    static bool has_any_connection();

private:
    void create_session(u64 request_id, u32 target_latency_ms) override;
    void finish_create_session(u64 request_id, ErrorOr<WebAudioRenderThread::OutputFormat>);
    void destroy_session(u64 session_id) override;

    void add_worklet_module(u64 session_id, u64 module_id, ByteString url, ByteString source_text) override;
    void set_render_graph(u64 session_id, ByteBuffer encoded_graph, Vector<Web::WebAudio::Render::SharedAudioBufferBinding> shared_audio_buffers) override;
    void set_suspended(u64 session_id, bool suspended, u64 generation) override;
    void set_media_element_audio_source_streams(u64 session_id, Vector<Web::WebAudio::Render::MediaElementAudioSourceStreamDescriptor> streams) override;
    void set_media_stream_audio_source_streams(u64 session_id, Vector<Web::WebAudio::Render::MediaStreamAudioSourceStreamDescriptor> streams) override;
    void set_script_processor_streams(u64 session_id, Vector<Web::WebAudio::Render::ScriptProcessorStreamDescriptor> streams) override;
    void set_worklet_node_ports(u64 session_id, Vector<Web::WebAudio::Render::WorkletNodePortDescriptor> ports) override;
    void set_worklet_node_definitions(u64 session_id, Vector<Web::WebAudio::Render::WorkletNodeDefinition> definitions) override;
    Messages::ToWebAudioWorkerFromSessionClient::CreateAnalyserStreamResponse create_analyser_stream(u64 session_id, u64 analyser_node_id, u32 fft_size, u32 block_count) override;
    Messages::ToWebAudioWorkerFromSessionClient::CreateDynamicsCompressorStreamResponse create_dynamics_compressor_stream(u64 session_id, u64 compressor_node_id, u32 block_count) override;

    // NOTE: This connection is created using socketpair() in WebAudioWorker and then one end is
    // transferred to the client process via SCM_RIGHTS. As a result, SO_PEERCRED reports WebAudioWorker
    // as the peer on this socket, not the actual remote process.
    Optional<int> m_socket_peer_pid;

    // Logical owner: the client process which requested this WebAudio connection.
    int m_owner_client_id { -1 };
    HashMap<u64, NonnullRefPtr<WebAudioSession>> m_webaudio_sessions;
    u64 m_next_webaudio_session_id { 1 };
};

}
