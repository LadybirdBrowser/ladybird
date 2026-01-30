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
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/SharedBufferStream.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/File.h>

#include <AudioServer/WebAudioClientEndpoint.h>
#include <AudioServer/WebAudioServerEndpoint.h>

#include <LibWeb/WebAudio/AudioParamDescriptor.h>
#include <LibWeb/WebAudio/Engine/StreamTransportDescriptors.h>
#include <LibWeb/WebAudio/Worklet/WorkletNodeDefinition.h>

namespace WebAudioWorkerClient {

class WebAudioClient final
    : public RefCounted<WebAudioClient>
    , public Weakable<WebAudioClient> {
public:
    struct WebAudioSession {
        u64 session_id { 0 };
        u32 sample_rate { 0 };
        u32 channel_count { 0 };
        Core::AnonymousBuffer timing_buffer;
        IPC::File timing_notify_fd;
    };

    static NonnullRefPtr<WebAudioClient> create();

    // Install a callback that can obtain a connected socket to the WebAudio server endpoint.
    // In production, this typically asks the broker process to mint a new connection.
    void set_socket_provider(Function<ErrorOr<IPC::File>(u64 page_id)>);

    ErrorOr<WebAudioSession> create_webaudio_session(u32 target_latency_ms, u64 page_id);
    ErrorOr<void> destroy_webaudio_session(u64 session_id);

    ErrorOr<void> webaudio_session_add_worklet_module(u64 session_id, u64 module_id, ByteString url, ByteString source_text);
    ErrorOr<void> webaudio_session_set_render_graph(u64 session_id, ByteBuffer encoded_graph);
    ErrorOr<void> webaudio_session_set_suspended(u64 session_id, bool suspended, u64 generation);
    ErrorOr<void> webaudio_session_set_media_element_audio_source_streams(u64 session_id, Vector<Web::WebAudio::Render::MediaElementAudioSourceStreamDescriptor> streams);
    ErrorOr<void> webaudio_session_set_media_stream_audio_source_streams(u64 session_id, Vector<Web::WebAudio::Render::MediaStreamAudioSourceStreamDescriptor> streams);
    ErrorOr<void> webaudio_session_set_script_processor_streams(u64 session_id, Vector<Web::WebAudio::Render::ScriptProcessorStreamDescriptor> streams);
    ErrorOr<void> webaudio_session_set_worklet_node_ports(u64 session_id, Vector<Web::WebAudio::Render::WorkletNodePortDescriptor> ports);
    ErrorOr<void> webaudio_session_set_worklet_node_definitions(u64 session_id, Vector<Web::WebAudio::Render::WorkletNodeDefinition> definitions);

    ErrorOr<Core::SharedBufferStream> webaudio_session_create_analyser_stream(u64 session_id, u64 analyser_node_id, u32 fft_size, u32 block_count);
    ErrorOr<Core::SharedBufferStream> webaudio_session_create_dynamics_compressor_stream(u64 session_id, u64 compressor_node_id, u32 block_count);

    Function<void()> on_death;
    Function<void(u64 session_id, u64 node_id)> on_worklet_processor_error;
    Function<void(u64 session_id, String const& name, Vector<Web::WebAudio::AudioParamDescriptor> const&, u64 generation)> on_worklet_processor_registered;
    Function<void(u64 session_id, u64 module_id, u64 required_generation, bool success, String const& error_name, String const& error_message, Vector<String> const& failed_processor_registrations)> on_worklet_module_evaluated;

private:
    WebAudioClient() = default;

    class WebAudioConnection final
        : public IPC::ConnectionToServer<WebAudioClientEndpoint, WebAudioServerEndpoint>
        , public WebAudioClientEndpoint {
        C_OBJECT_ABSTRACT(WebAudioConnection);

    public:
        explicit WebAudioConnection(WebAudioClient&, NonnullOwnPtr<IPC::Transport>);

        Function<void()> on_death;

    private:
        void die() override;
        void webaudio_session_worklet_processor_error(u64 session_id, u64 node_id) override;
        void webaudio_session_worklet_processor_registered(u64 session_id, String name, Vector<Web::WebAudio::AudioParamDescriptor> descriptors, u64 generation) override;
        void webaudio_session_worklet_module_evaluated(u64 session_id, u64 module_id, u64 required_generation, bool success, String error_name, String error_message, Vector<String> failed_processor_registrations) override;

        WebAudioClient& m_client;
    };

    ErrorOr<void> ensure_connection(u64 page_id);

    RefPtr<WebAudioConnection> m_connection;
    Function<ErrorOr<IPC::File>(u64 page_id)> m_socket_provider;
};

}
