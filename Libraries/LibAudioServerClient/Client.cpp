/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibAudioServerClient/Client.h>

#include <LibCore/EventLoop.h>
#include <LibThreading/Mutex.h>

namespace AudioServerClient {

static Threading::Mutex s_default_client_mutex;
static RefPtr<Client> s_default_client;

void Client::set_default_client(RefPtr<Client> client)
{
    Threading::MutexLocker locker { s_default_client_mutex };
    s_default_client = move(client);
}

RefPtr<Client> Client::default_client()
{
    Threading::MutexLocker locker { s_default_client_mutex };
    return s_default_client;
}

Client::Client(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<AudioServerClientEndpoint, AudioServerServerEndpoint>(*this, move(transport))
{
}

void Client::die()
{
    auto self = NonnullRefPtr(*this);

    {
        Threading::MutexLocker locker { m_output_device_format_mutex };
        m_cached_output_device_format = {};
    }

    auto death_callback = move(on_death);
    on_death = {};
    if (death_callback)
        death_callback();
}

ErrorOr<void> Client::set_muted(bool muted)
{
    auto response = send_sync_but_allow_failure<Messages::AudioServerServer::SetMuted>(muted);
    if (!response)
        return Error::from_string_literal("AudioServerClient: set muted IPC failed");
    return {};
}

ErrorOr<Core::SharedSingleProducerCircularBuffer> Client::create_shared_single_producer_circular_buffer(size_t capacity)
{
    auto response = send_sync_but_allow_failure<Messages::AudioServerServer::CreateSharedSingleProducerCircularBuffer>(capacity);
    if (!response)
        return Error::from_string_literal("AudioServerClient: create buffer IPC failed");

    auto buffer = response->shm_buffer();
    if (!buffer.is_valid())
        return Error::from_string_literal("AudioServerClient: server returned invalid buffer");

    return Core::SharedSingleProducerCircularBuffer::attach(move(buffer));
}

ErrorOr<Core::SharedBufferStream> Client::create_shared_buffer_stream(u32 block_size, u32 block_count)
{
    auto response = send_sync_but_allow_failure<Messages::AudioServerServer::CreateSharedBufferStream>(block_size, block_count);
    if (!response)
        return Error::from_string_literal("AudioServerClient: create shared buffer stream IPC failed");

    auto pool_buffer = response->pool_buffer();
    auto ready_ring_buffer = response->ready_ring_buffer();
    auto free_ring_buffer = response->free_ring_buffer();

    if (!pool_buffer.is_valid() || !ready_ring_buffer.is_valid() || !free_ring_buffer.is_valid())
        return Error::from_string_literal("AudioServerClient: server returned invalid shared buffer stream buffers");

    return Core::SharedBufferStream::attach(move(pool_buffer), move(ready_ring_buffer), move(free_ring_buffer));
}

ErrorOr<Client::AudioOutputSession> Client::create_audio_output_session(u32 target_latency_ms, AudioServer::AudioOutputDeviceID device_id)
{
    auto response = send_sync_but_allow_failure<Messages::AudioServerServer::CreateAudioOutputSession>(target_latency_ms, device_id);
    if (!response)
        return Error::from_string_literal("AudioServerClient: create audio output session IPC failed");

    auto session_id = response->session_id();
    auto sample_rate = response->sample_rate();
    auto channel_count = response->channel_count();
    auto buffer = response->shm_buffer();

    if (session_id == 0 || sample_rate == 0 || channel_count == 0)
        return Error::from_string_literal("AudioServerClient: server returned invalid audio session");
    if (!buffer.is_valid())
        return Error::from_string_literal("AudioServerClient: server returned invalid audio ring buffer");

    return AudioOutputSession {
        .session_id = session_id,
        .sample_rate = sample_rate,
        .channel_count = channel_count,
        .ring = TRY(Core::SharedSingleProducerCircularBuffer::attach(move(buffer))),
    };
}

ErrorOr<u64> Client::create_audio_output_session_async(u32 target_latency_ms, AudioServer::AudioOutputDeviceID device_id)
{
    auto response = send_sync_but_allow_failure<Messages::AudioServerServer::CreateAudioOutputSessionAsync>(target_latency_ms, device_id);
    if (!response)
        return Error::from_string_literal("AudioServerClient: create audio output session (async) IPC failed");

    auto session_id = response->session_id();
    if (session_id == 0)
        return Error::from_string_literal("AudioServerClient: server returned invalid audio session id");
    return session_id;
}

void Client::audio_output_session_ready(u64 session_id, u32 sample_rate, u32 channel_count, Core::AnonymousBuffer shm_buffer)
{
    if (!on_audio_output_session_ready)
        return;

    if (session_id == 0 || sample_rate == 0 || channel_count == 0) {
        if (on_audio_output_session_failed)
            on_audio_output_session_failed(session_id, ByteString("Server returned invalid audio session format"));
        return;
    }

    if (!shm_buffer.is_valid()) {
        if (on_audio_output_session_failed)
            on_audio_output_session_failed(session_id, ByteString("Server returned invalid audio ring buffer"));
        return;
    }

    auto ring_or_error = Core::SharedSingleProducerCircularBuffer::attach(move(shm_buffer));
    if (ring_or_error.is_error()) {
        if (on_audio_output_session_failed)
            on_audio_output_session_failed(session_id, ByteString("Failed to attach audio ring buffer"));
        return;
    }

    on_audio_output_session_ready(AudioOutputSession {
        .session_id = session_id,
        .sample_rate = sample_rate,
        .channel_count = channel_count,
        .ring = ring_or_error.release_value(),
    });
}

void Client::audio_output_session_failed(u64 session_id, ByteString error)
{
    if (on_audio_output_session_failed)
        on_audio_output_session_failed(session_id, move(error));
}

ErrorOr<void> Client::destroy_audio_output_session(u64 session_id)
{
    // IPC::ConnectionBase::send_sync_* drains peer messages and uses Core::deferred_invoke,
    // which requires a Core::EventLoop on the calling thread.
    // During shutdown (or from auxiliary threads) that may not be true, so fall back to
    // fire-and-forget and let connection teardown clean up if needed.
    if (!Core::EventLoop::is_running()) {
        (void)post_message(Messages::AudioServerServer::DestroyAudioOutputSession(session_id));
        return {};
    }

    auto response = send_sync_but_allow_failure<Messages::AudioServerServer::DestroyAudioOutputSession>(session_id);
    if (!response)
        return Error::from_string_literal("AudioServerClient: destroy audio output session IPC failed");
    return {};
}

ErrorOr<Client::OutputDeviceFormat> Client::get_output_device_format()
{
    {
        Threading::MutexLocker locker { m_output_device_format_mutex };
        if (m_cached_output_device_format.has_value())
            return m_cached_output_device_format.value();
    }

    auto response = send_sync_but_allow_failure<Messages::AudioServerServer::GetOutputDeviceFormat>();
    if (!response)
        return Error::from_string_literal("AudioServerClient: get output device format IPC failed");

    OutputDeviceFormat format {
        .sample_rate = response->sample_rate(),
        .channel_count = response->channel_count(),
    };

    if (format.sample_rate == 0 || format.channel_count == 0)
        return Error::from_string_literal("AudioServerClient: server returned invalid output device format");

    {
        Threading::MutexLocker locker { m_output_device_format_mutex };
        m_cached_output_device_format = format;
    }

    return format;
}

ErrorOr<Vector<AudioServer::AudioInputDeviceInfo>> Client::get_audio_input_devices()
{
    (void)this;
    auto response = send_sync_but_allow_failure<Messages::AudioServerServer::GetAudioInputDevices>();
    if (!response)
        return Error::from_string_literal("AudioServerClient: get audio input devices IPC failed");

    return response->devices();
}

ErrorOr<Vector<AudioServer::AudioOutputDeviceInfo>> Client::get_audio_output_devices()
{
    auto response = send_sync_but_allow_failure<Messages::AudioServerServer::GetAudioOutputDevices>();
    if (!response)
        return Error::from_string_literal("AudioServerClient: get audio output devices IPC failed");

    return response->devices();
}

ErrorOr<AudioServer::AudioInputStreamDescriptor> Client::create_audio_input_stream(AudioServer::AudioInputDeviceID device_id, u32 sample_rate_hz, u32 channel_count, u64 capacity_frames, AudioServer::StreamOverflowPolicy overflow_policy)
{
    auto response = send_sync_but_allow_failure<Messages::AudioServerServer::CreateAudioInputStream>(device_id, sample_rate_hz, channel_count, capacity_frames, static_cast<u8>(overflow_policy));
    if (!response)
        return Error::from_string_literal("AudioServerClient: create audio input stream IPC failed");

    auto descriptor = response->take_descriptor();
    if (descriptor.stream_id == 0 || !descriptor.shared_memory.is_valid())
        return Error::from_string_literal("AudioServerClient: server returned invalid audio input stream descriptor");

    return move(descriptor);
}

ErrorOr<void> Client::destroy_audio_input_stream(AudioServer::AudioInputStreamID stream_id)
{
    if (stream_id == 0)
        return Error::from_string_literal("AudioServerClient: invalid audio input stream id");

    auto response = send_sync_but_allow_failure<Messages::AudioServerServer::DestroyAudioInputStream>(stream_id);
    if (!response)
        return Error::from_string_literal("AudioServerClient: destroy audio input stream IPC failed");
    return {};
}

}
