/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <AudioServer/AudioInputDeviceInfo.h>
#include <AudioServer/AudioInputStreamDescriptor.h>
#include <AudioServer/WebAudioClientEndpoint.h>
#include <AudioServer/WebAudioServerEndpoint.h>
#include <LibIPC/ConnectionFromClient.h>
#include <LibWeb/WebAudio/Engine/StreamTransportDescriptors.h>
#include <LibWeb/WebAudio/Worklet/WorkletNodeDefinition.h>
#include <WebAudioWorker/WebAudioSession.h>

namespace WebAudioWorker {

class WebAudioConnection final
    : public IPC::ConnectionFromClient<WebAudioClientEndpoint, WebAudioServerEndpoint> {
    C_OBJECT(WebAudioConnection);

public:
    WebAudioConnection(NonnullOwnPtr<IPC::Transport>, int owner_client_id);
    ~WebAudioConnection() override;

    void die() override;

    static bool has_any_connection();

private:
    Messages::WebAudioServer::GetOutputDeviceFormatResponse get_output_device_format() override;
    Messages::WebAudioServer::CreateWebaudioSessionResponse create_webaudio_session(u32 target_latency_ms) override;
    void destroy_webaudio_session(u64 session_id) override;

    void webaudio_session_add_worklet_module(u64 session_id, u64 module_id, ByteString url, ByteString source_text) override;
    void webaudio_session_set_render_graph(u64 session_id, ByteBuffer encoded_graph) override;
    void webaudio_session_set_suspended(u64 session_id, bool suspended, u64 generation) override;
    void webaudio_session_set_media_element_audio_source_streams(u64 session_id, Vector<Web::WebAudio::Render::MediaElementAudioSourceStreamDescriptor> streams) override;
    void webaudio_session_set_media_stream_audio_source_streams(u64 session_id, Vector<Web::WebAudio::Render::MediaStreamAudioSourceStreamDescriptor> streams) override;
    void webaudio_session_set_script_processor_streams(u64 session_id, Vector<Web::WebAudio::Render::ScriptProcessorStreamDescriptor> streams) override;
    void webaudio_session_set_worklet_node_ports(u64 session_id, Vector<Web::WebAudio::Render::WorkletNodePortDescriptor> ports) override;
    void webaudio_session_set_worklet_node_definitions(u64 session_id, Vector<Web::WebAudio::Render::WorkletNodeDefinition> definitions) override;
    Messages::WebAudioServer::WebaudioSessionCreateAnalyserStreamResponse webaudio_session_create_analyser_stream(u64 session_id, u64 analyser_node_id, u32 fft_size, u32 block_count) override;
    Messages::WebAudioServer::WebaudioSessionCreateDynamicsCompressorStreamResponse webaudio_session_create_dynamics_compressor_stream(u64 session_id, u64 compressor_node_id, u32 block_count) override;

    Messages::WebAudioServer::WebaudioSessionCreateAudioInputStreamResponse webaudio_session_create_audio_input_stream(u64 session_id, AudioServer::AudioInputDeviceID device_id, u32 sample_rate_hz, u32 channel_count, u64 capacity_frames, u8 overflow_policy) override;
    void webaudio_session_destroy_audio_input_stream(u64 session_id, AudioServer::AudioInputStreamID stream_id) override;

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
