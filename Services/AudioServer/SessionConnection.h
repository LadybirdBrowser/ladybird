/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/ByteBuffer.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <AudioServer/InputStream.h>
#include <AudioServer/OutputStream.h>
#include <LibAudioServer/SharedCircularBuffer.h>
#include <LibAudioServer/ToAudioServerFromSessionClientEndpoint.h>
#include <LibAudioServer/ToSessionClientFromAudioServerEndpoint.h>
#include <LibIPC/ConnectionFromClient.h>
#include <thread>

namespace Core {

class ThreadEventQueue;

}

namespace AudioServer {

class SessionConnection final
    : public IPC::ConnectionFromClient<ToSessionClientFromAudioServerEndpoint, ToAudioServerFromSessionClientEndpoint> {
    C_OBJECT(SessionConnection);

public:
    ~SessionConnection() override;

    void die() override;

    static ErrorOr<IPC::File> connect_new_client_for_broker();

private:
    void verify_thread_affinity() const { VERIFY(m_creation_thread == std::this_thread::get_id()); }
    friend class Server;

    explicit SessionConnection(NonnullOwnPtr<IPC::Transport>);

    Messages::ToAudioServerFromSessionClient::InitTransportResponse init_transport(int peer_pid) override;
    void get_devices(u64 request_token) override;
    void create_input_stream(u64 request_token, ByteString grant_id, DeviceHandle device_handle, u64 capacity_frames) override;
    void start_input_stream(u64 request_token, u64 stream_id) override;
    void stop_input_stream(u64 request_token, u64 stream_id) override;
    void create_session(u64 request_token, ByteString grant_id, u32 target_latency_ms, DeviceHandle device_handle) override;
    void destroy_session(u64 request_token, u64 session_id) override;
    void destroy_input_stream(u64 request_token, u64 stream_id) override;
    void set_output_sink_volume(u64 request_token, u64 session_id, double gain) override;

    struct OutputSink {
        struct State : public RefCounted<State> {
            Atomic<bool> ring_ready { false };
            Optional<AudioServer::SharedCircularBuffer> ring;
            Optional<Core::AnonymousBuffer> timing_buffer;
            size_t bytes_per_frame { 0 };
            Atomic<u64> underrun_frames { 0 };
        };

        u64 id { 0 };
        DeviceHandle device_handle { 0 };
        u32 target_latency_ms { 0 };
        u32 sample_rate { 0 };
        u32 channel_count { 0 };
        float gain { 1.0f };
        NonnullRefPtr<State> state;
    };

    HashMap<u64, OutputSink> m_output_sinks;

    struct InputSource {
        u64 id { 0 };
        DeviceHandle device_handle { 0 };
        ByteString grant_id;
        bool started { true };
        RefPtr<AudioServer::InputStream> stream;
    };

    HashMap<u64, InputSource> m_input_streams;

    Core::ThreadEventQueue* m_control_event_queue { nullptr };

    void finalize_output_sink(u64 session_id);
    void stop_all_streams_for_grant_revocation(ByteString const& grant_id);
    void destroy_output_sink_internal(u64 session_id);
    void destroy_input_stream_internal(u64 stream_id);
    void fail_output_sink(u64 session_id, ByteString const& error);
    void notify_devices_changed();
    std::thread::id m_creation_thread { std::this_thread::get_id() };
};

}
