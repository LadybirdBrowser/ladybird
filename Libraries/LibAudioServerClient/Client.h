/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AudioServer/AudioInputDeviceInfo.h>
#include <AudioServer/AudioInputStreamDescriptor.h>
#include <AudioServer/AudioOutputDeviceInfo.h>
#include <AudioServer/AudioServerClientEndpoint.h>
#include <AudioServer/AudioServerServerEndpoint.h>
#include <LibCore/SharedBufferStream.h>
#include <LibCore/SharedSingleProducerCircularBuffer.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/File.h>
#include <LibThreading/Mutex.h>

namespace AudioServerClient {

class Client final
    : public IPC::ConnectionToServer<AudioServerClientEndpoint, AudioServerServerEndpoint>
    , public AudioServerClientEndpoint {
    C_OBJECT_ABSTRACT(Client);

public:
    using InitTransport = Messages::AudioServerServer::InitTransport;

    struct AudioOutputSession {
        u64 session_id { 0 };
        u32 sample_rate { 0 };
        u32 channel_count { 0 };
        Core::SharedSingleProducerCircularBuffer ring;
    };

    struct OutputDeviceFormat {
        u32 sample_rate { 0 };
        u32 channel_count { 0 };
    };

    explicit Client(NonnullOwnPtr<IPC::Transport>);

    static void set_default_client(RefPtr<Client>);
    static RefPtr<Client> default_client();

    ErrorOr<Core::SharedSingleProducerCircularBuffer> create_shared_single_producer_circular_buffer(size_t capacity);
    ErrorOr<Core::SharedBufferStream> create_shared_buffer_stream(u32 block_size, u32 block_count);

    ErrorOr<AudioOutputSession> create_audio_output_session(u32 target_latency_ms, AudioServer::AudioOutputDeviceID device_id = 0);
    ErrorOr<u64> create_audio_output_session_async(u32 target_latency_ms, AudioServer::AudioOutputDeviceID device_id = 0);
    ErrorOr<void> destroy_audio_output_session(u64 session_id);

    Function<void(AudioOutputSession)> on_audio_output_session_ready;
    Function<void(u64 session_id, ByteString error)> on_audio_output_session_failed;

    ErrorOr<OutputDeviceFormat> get_output_device_format();

    ErrorOr<Vector<AudioServer::AudioOutputDeviceInfo>> get_audio_output_devices();

    ErrorOr<Vector<AudioServer::AudioInputDeviceInfo>> get_audio_input_devices();

    ErrorOr<AudioServer::AudioInputStreamDescriptor> create_audio_input_stream(AudioServer::AudioInputDeviceID device_id, u32 sample_rate_hz, u32 channel_count, u64 capacity_frames, AudioServer::StreamOverflowPolicy overflow_policy);
    ErrorOr<void> destroy_audio_input_stream(AudioServer::AudioInputStreamID stream_id);

    // Connection-scoped: mute/unmute all audio produced by sessions owned by this client.
    // The server still drains the session rings so internal timing continues to advance.
    ErrorOr<void> set_muted(bool muted);

    Function<void()> on_death;

private:
    void die() override;

    void audio_output_session_ready(u64 session_id, u32 sample_rate, u32 channel_count, Core::AnonymousBuffer shm_buffer) override;
    void audio_output_session_failed(u64 session_id, ByteString error) override;

    Threading::Mutex m_output_device_format_mutex;
    Optional<OutputDeviceFormat> m_cached_output_device_format;
};

}
