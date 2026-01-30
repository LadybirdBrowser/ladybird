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
#include <AK/Vector.h>
#include <AudioServer/AudioInputDeviceInfo.h>
#include <AudioServer/AudioInputStreamDescriptor.h>
#include <AudioServer/AudioOutputDeviceInfo.h>
#include <AudioServer/AudioServerClientEndpoint.h>
#include <AudioServer/AudioServerServerEndpoint.h>
#include <AudioServer/Forward.h>
#include <LibCore/SharedSingleProducerCircularBuffer.h>
#include <LibIPC/ConnectionFromClient.h>

namespace Core {

class ThreadEventQueue;

}

namespace AudioServer {

class AudioServerConnection final
    : public IPC::ConnectionFromClient<AudioServerClientEndpoint, AudioServerServerEndpoint> {
    C_OBJECT(AudioServerConnection);

public:
    ~AudioServerConnection() override;

    void die() override;

    static void disconnect_all_clients_due_to_output_device_format_change(u32 old_sample_rate, u32 old_channel_count, u32 new_sample_rate, u32 new_channel_count);

private:
    explicit AudioServerConnection(NonnullOwnPtr<IPC::Transport>);

    Messages::AudioServerServer::InitTransportResponse init_transport(int peer_pid) override;
    Messages::AudioServerServer::CreateSharedSingleProducerCircularBufferResponse create_shared_single_producer_circular_buffer(size_t capacity) override;
    Messages::AudioServerServer::CreateSharedBufferStreamResponse create_shared_buffer_stream(u32 block_size, u32 block_count) override;

    Messages::AudioServerServer::GetOutputDeviceFormatResponse get_output_device_format() override;

    Messages::AudioServerServer::GetAudioOutputDevicesResponse get_audio_output_devices() override;

    Messages::AudioServerServer::GetAudioInputDevicesResponse get_audio_input_devices() override;

    Messages::AudioServerServer::CreateAudioInputStreamResponse create_audio_input_stream(AudioInputDeviceID device_id, u32 sample_rate_hz, u32 channel_count, u64 capacity_frames, u8 overflow_policy) override;
    void destroy_audio_input_stream(AudioInputStreamID stream_id) override;

    void set_muted(bool muted) override;

    Messages::AudioServerServer::CreateAudioOutputSessionResponse create_audio_output_session(u32 target_latency_ms, AudioOutputDeviceID device_id) override;
    Messages::AudioServerServer::CreateAudioOutputSessionAsyncResponse create_audio_output_session_async(u32 target_latency_ms, AudioOutputDeviceID device_id) override;
    void destroy_audio_output_session(u64 session_id) override;

    Messages::AudioServerServer::ConnectNewClientsResponse connect_new_clients(size_t count) override;

    static ErrorOr<IPC::File> connect_new_client();

    struct AudioOutputSession {
        struct State : public RefCounted<State> {
            Atomic<bool> ring_ready { false };
            Optional<Core::SharedSingleProducerCircularBuffer> ring;
            size_t bytes_per_frame { 0 };
            Atomic<u64> underrun_frames { 0 };
        };

        u64 id { 0 };
        u32 target_latency_ms { 0 };
        u32 sample_rate { 0 };
        u32 channel_count { 0 };
        NonnullRefPtr<State> state;
    };

    HashMap<u64, AudioOutputSession> m_audio_output_sessions;

    struct AudioInputStream {
        AudioInputStreamID id { 0 };
        AudioInputDeviceID device_id { 0 };
    };

    HashMap<AudioInputStreamID, AudioInputStream> m_audio_input_streams;

    static Atomic<u64> s_next_audio_output_session_id;

    Core::ThreadEventQueue* m_control_event_queue { nullptr };

    bool m_is_muted { false };

    void finalize_audio_output_session_async(u64 session_id);
};

}
