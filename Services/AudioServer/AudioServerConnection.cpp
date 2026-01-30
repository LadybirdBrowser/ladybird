/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IDAllocator.h>
#include <AK/Math.h>
#include <AK/Time.h>
#include <AudioServer/AudioInputDeviceManager.h>
#include <AudioServer/AudioInputRingStream.h>
#include <AudioServer/AudioInputStreamManager.h>
#include <AudioServer/AudioOutputDevice.h>
#include <AudioServer/AudioOutputDeviceManager.h>
#include <AudioServer/AudioServerConnection.h>
#include <AudioServer/ConnectionLifecycle.h>
#include <AudioServer/Debug.h>
#include <LibCore/EventLoop.h>
#include <LibCore/SharedBufferStream.h>
#include <LibCore/SharedSingleProducerCircularBuffer.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibCore/ThreadEventQueue.h>
#include <LibThreading/ConditionVariable.h>
#include <bit>

namespace AudioServer {

Atomic<u64> AudioServerConnection::s_next_audio_output_session_id { 1 };

static HashMap<int, RefPtr<AudioServerConnection>> s_connections;
static IDAllocator s_client_ids;

AudioServerConnection::AudioServerConnection(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<AudioServerClientEndpoint, AudioServerServerEndpoint>(*this, move(transport), s_client_ids.allocate())
{
    m_control_event_queue = &Core::ThreadEventQueue::current();

    register_connection();
    s_connections.set(client_id(), *this);
}

AudioServerConnection::~AudioServerConnection()
{
    for (auto const& it : m_audio_output_sessions) {
        auto const& session = it.value;
        if (session.state->ring_ready.load(AK::MemoryOrder::memory_order_acquire))
            AudioOutputDevice::the().unregister_producer(session.id);
    }
    m_audio_output_sessions.clear();

    for (auto const& it : m_audio_input_streams)
        AudioInputStreamManager::destroy_stream(it.key);
    m_audio_input_streams.clear();
}

void AudioServerConnection::die()
{
    auto id = client_id();

    if (should_log_audio_server_ipc())
        dbgln("cid={}: AudioServerConnection::die()", id);

    s_connections.remove(id);
    s_client_ids.deallocate(id);
    unregister_connection();
}

Messages::AudioServerServer::InitTransportResponse AudioServerConnection::init_transport([[maybe_unused]] int peer_pid)
{
    VERIFY_NOT_REACHED();
}

Messages::AudioServerServer::CreateSharedSingleProducerCircularBufferResponse AudioServerConnection::create_shared_single_producer_circular_buffer(size_t capacity)
{
    auto shared_buffer_or_error = Core::SharedSingleProducerCircularBuffer::create(capacity);
    if (shared_buffer_or_error.is_error()) {
        warnln("cid={}: failed to create shared circular buffer: {}", client_id(), shared_buffer_or_error.error());
        return { Core::AnonymousBuffer {} };
    }

    return { shared_buffer_or_error.release_value().anonymous_buffer() };
}

struct SharedBufferStreamBuffers {
    Core::AnonymousBuffer pool_buffer;
    Core::AnonymousBuffer ready_ring_buffer;
    Core::AnonymousBuffer free_ring_buffer;
};

static Optional<SharedBufferStreamBuffers> create_shared_buffer_stream_buffers(u32 block_size, u32 block_count, char const* debug_name, int client_id)
{
    if (block_size == 0 || block_count == 0)
        return {};

    auto pool_bytes = Core::SharedBufferStream::pool_buffer_size_bytes(block_size, block_count);
    auto pool_buffer_or_error = Core::AnonymousBuffer::create_with_size(pool_bytes);
    if (pool_buffer_or_error.is_error()) {
        warnln("cid={}: failed to allocate {} stream pool: {}", client_id, debug_name, pool_buffer_or_error.error());
        return {};
    }

    auto pool_buffer = pool_buffer_or_error.release_value();
    auto* header = reinterpret_cast<Core::SharedBufferStream::PoolHeader*>(pool_buffer.data<void>());
    if (!header)
        return {};

    __builtin_memset(header, 0, sizeof(*header));
    header->magic = Core::SharedBufferStream::s_pool_magic;
    header->version = Core::SharedBufferStream::s_pool_version;
    header->block_size = block_size;
    header->block_count = block_count;

    size_t ring_capacity_bytes = std::bit_ceil(static_cast<size_t>(block_count) * sizeof(Core::SharedBufferStream::Descriptor));
    ring_capacity_bytes = max(ring_capacity_bytes, 64ul);

    auto ready_ring_or_error = Core::SharedSingleProducerCircularBuffer::create(ring_capacity_bytes);
    if (ready_ring_or_error.is_error()) {
        warnln("cid={}: failed to create {} stream ready ring: {}", client_id, debug_name, ready_ring_or_error.error());
        return {};
    }

    auto free_ring_or_error = Core::SharedSingleProducerCircularBuffer::create(ring_capacity_bytes);
    if (free_ring_or_error.is_error()) {
        warnln("cid={}: failed to create {} stream free ring: {}", client_id, debug_name, free_ring_or_error.error());
        return {};
    }

    auto ready_ring = ready_ring_or_error.release_value();
    auto free_ring = free_ring_or_error.release_value();

    for (u32 i = 0; i < block_count; ++i) {
        Core::SharedBufferStream::Descriptor desc { i, 0 };
        auto bytes = ReadonlyBytes { reinterpret_cast<u8 const*>(&desc), sizeof(desc) };
        if (free_ring.try_write(bytes) != sizeof(desc)) {
            warnln("cid={}: failed to seed {} stream free ring (i={})", client_id, debug_name, i);
            return {};
        }
    }

    return SharedBufferStreamBuffers {
        .pool_buffer = move(pool_buffer),
        .ready_ring_buffer = ready_ring.anonymous_buffer(),
        .free_ring_buffer = free_ring.anonymous_buffer(),
    };
}

struct OutputDeviceFormat {
    u32 sample_rate { 0 };
    u32 channel_count { 0 };
};

static Optional<OutputDeviceFormat> wait_for_output_device_format(AudioOutputDevice& device)
{
    // Do not hang forever. If we cannot obtain a sample specification promptly, treat this as fatal.
    //
    // Note: Avoid blocking the AudioServer control thread waiting on AudioOutputDevice::when_ready().
    // The sample specification callback posts notify_ready() via deferred_invoke() onto this thread's
    // ThreadEventQueue, which requires the control thread to keep running. Waiting on a condition
    // variable here can prevent the deferred invoke from being processed and effectively guarantees
    // hitting the timeout.
    AK::MonotonicTime deadline = AK::MonotonicTime::now() + AK::Duration::from_seconds(2);
    while (!device.has_sample_specification() && AK::MonotonicTime::now() < deadline) {
        auto sleep_or_error = Core::System::sleep_ms(1);
        if (sleep_or_error.is_error())
            return {};
    }

    if (!device.has_sample_specification())
        return {};

    u32 sample_rate = device.device_sample_rate_hz();
    u32 channel_count = device.device_channel_count();
    if (sample_rate == 0 || channel_count == 0)
        return {};

    return OutputDeviceFormat {
        .sample_rate = sample_rate,
        .channel_count = channel_count,
    };
}

static ErrorOr<Core::SharedSingleProducerCircularBuffer> create_audio_output_ring(u32 sample_rate, u32 channel_count, u32 target_latency_ms)
{
    if (sample_rate == 0 || channel_count == 0)
        return Error::from_string_literal("invalid sample specification");

    // Allocate enough buffering for several callback buffers.
    // The ring buffer's capacity must be a power of two (in bytes).
    // Note: The playback backend's callback buffer size is not exposed here, so use a conservative minimum.
    constexpr size_t minimum_callback_frames = 128;
    size_t bytes_per_frame = channel_count * sizeof(float);
    size_t minimum_callback_bytes = minimum_callback_frames * bytes_per_frame;
    size_t desired_latency_frames = static_cast<size_t>((static_cast<double>(sample_rate) * static_cast<double>(target_latency_ms)) / 1000.0);
    size_t desired_latency_bytes = max(desired_latency_frames * bytes_per_frame, minimum_callback_bytes * 2);

    // Round up to a power of two, with a minimum to avoid tiny rings.
    size_t ring_capacity_bytes = std::bit_ceil(desired_latency_bytes);
    ring_capacity_bytes = max(ring_capacity_bytes, minimum_callback_bytes * 8);

    return Core::SharedSingleProducerCircularBuffer::create(ring_capacity_bytes);
}

Messages::AudioServerServer::CreateSharedBufferStreamResponse AudioServerConnection::create_shared_buffer_stream(u32 block_size, u32 block_count)
{
    auto buffers = create_shared_buffer_stream_buffers(block_size, block_count, "shared buffer", client_id());
    if (!buffers.has_value())
        return { Core::AnonymousBuffer {}, Core::AnonymousBuffer {}, Core::AnonymousBuffer {} };

    return { move(buffers->pool_buffer), move(buffers->ready_ring_buffer), move(buffers->free_ring_buffer) };
}

Messages::AudioServerServer::GetOutputDeviceFormatResponse AudioServerConnection::get_output_device_format()
{
    if (should_log_audio_server())
        dbgln("cid={}: get_output_device_format()", client_id());

    AudioOutputDevice& device = AudioOutputDevice::the();
    if (m_control_event_queue)
        device.ensure_started(*m_control_event_queue, 50);

    auto format = wait_for_output_device_format(device);
    if (!format.has_value()) {
        warnln("cid={}: audio output device did not provide a sample specification", client_id());
        return { 0, 0 };
    }

    return { format->sample_rate, format->channel_count };
}

Messages::AudioServerServer::GetAudioInputDevicesResponse AudioServerConnection::get_audio_input_devices()
{
    if (should_log_audio_server())
        dbgln("cid={}: get_audio_input_devices()", client_id());

    auto devices = AudioInputDeviceManager::enumerate_devices();
    return { move(devices) };
}

Messages::AudioServerServer::GetAudioOutputDevicesResponse AudioServerConnection::get_audio_output_devices()
{
    if (should_log_audio_server())
        dbgln("cid={}: get_audio_output_devices()", client_id());

    auto devices = AudioOutputDeviceManager::enumerate_devices();
    return { move(devices) };
}

Messages::AudioServerServer::CreateAudioInputStreamResponse AudioServerConnection::create_audio_input_stream(AudioInputDeviceID device_id, u32 sample_rate_hz, u32 channel_count, u64 capacity_frames, u8 overflow_policy)
{
    if (should_log_audio_server())
        dbgln("cid={}: create_audio_input_stream(device_id={}, sample_rate_hz={}, channel_count={}, capacity_frames={}, overflow_policy={})", client_id(), device_id, sample_rate_hz, channel_count, capacity_frames, overflow_policy);

    StreamOverflowPolicy policy = StreamOverflowPolicy::DropOldest;
    if (overflow_policy <= static_cast<u8>(StreamOverflowPolicy::Lossless))
        policy = static_cast<StreamOverflowPolicy>(overflow_policy);

    auto descriptor_or_error = AudioInputStreamManager::create_stream(device_id, sample_rate_hz, channel_count, capacity_frames, policy);
    if (descriptor_or_error.is_error())
        return { AudioInputStreamDescriptor {} };

    auto descriptor = descriptor_or_error.release_value();
    if (descriptor.stream_id == 0)
        return { AudioInputStreamDescriptor {} };

    AudioInputStream stream;
    stream.id = descriptor.stream_id;
    stream.device_id = device_id;
    m_audio_input_streams.set(stream.id, stream);

    return { move(descriptor) };
}

void AudioServerConnection::destroy_audio_input_stream(AudioInputStreamID stream_id)
{
    if (should_log_audio_server())
        dbgln("cid={}: destroy_audio_input_stream(stream_id={})", client_id(), stream_id);
    m_audio_input_streams.remove(stream_id);
    AudioInputStreamManager::destroy_stream(stream_id);
}

void AudioServerConnection::set_muted(bool muted)
{
    if (should_log_audio_server())
        dbgln("cid={}: set_muted({})", client_id(), muted);

    m_is_muted = muted;

    AudioOutputDevice& device = AudioOutputDevice::the();
    for (auto const& it : m_audio_output_sessions)
        device.set_producer_muted(it.key, muted);
}

Messages::AudioServerServer::CreateAudioOutputSessionResponse AudioServerConnection::create_audio_output_session(u32 target_latency_ms, AudioOutputDeviceID device_id)
{
    if (should_log_audio_server())
        dbgln("cid={}: create_audio_output_session(target_latency_ms={}, device_id={})", client_id(), target_latency_ms, device_id);

    if (device_id != 0 && should_log_audio_server())
        dbgln("cid={}: create_audio_output_session: device selection not implemented, using default output", client_id());

    u64 session_id = s_next_audio_output_session_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);

    auto state = adopt_ref(*new AudioOutputSession::State);

    AudioOutputDevice& device = AudioOutputDevice::the();
    if (m_control_event_queue)
        device.ensure_started(*m_control_event_queue, target_latency_ms);

    auto format = wait_for_output_device_format(device);
    if (!format.has_value()) {
        warnln("cid={}: audio output device did not provide a sample specification", client_id());
        return { 0, 0, 0, Core::AnonymousBuffer {} };
    }

    u32 sample_rate = format->sample_rate;
    u32 channel_count = format->channel_count;

    auto ring_or_error = create_audio_output_ring(sample_rate, channel_count, target_latency_ms);
    if (ring_or_error.is_error()) {
        warnln("cid={}: failed to create audio output ring: {}", client_id(), ring_or_error.error());
        return { 0, 0, 0, Core::AnonymousBuffer {} };
    }

    auto ring = ring_or_error.release_value();

    auto shm_buffer = ring.anonymous_buffer();

    size_t bytes_per_frame = static_cast<size_t>(channel_count) * sizeof(float);

    state->ring = ring;
    state->bytes_per_frame = bytes_per_frame;
    state->ring_ready.store(true, AK::MemoryOrder::memory_order_release);

    device.register_producer(session_id, ring, bytes_per_frame);
    device.set_producer_muted(session_id, m_is_muted);

    if (should_log_audio_server())
        dbgln("cid={}: audio output session {} ready ({} Hz, {} channels)", client_id(), session_id, sample_rate, channel_count);

    AudioOutputSession session {
        .id = session_id,
        .target_latency_ms = target_latency_ms,
        .sample_rate = sample_rate,
        .channel_count = channel_count,
        .state = state,
    };

    m_audio_output_sessions.set(session_id, move(session));

    return { session_id, sample_rate, channel_count, move(shm_buffer) };
}

Messages::AudioServerServer::CreateAudioOutputSessionAsyncResponse AudioServerConnection::create_audio_output_session_async(u32 target_latency_ms, AudioOutputDeviceID device_id)
{
    if (should_log_audio_server())
        dbgln("cid={}: create_audio_output_session_async(target_latency_ms={}, device_id={})", client_id(), target_latency_ms, device_id);

    if (device_id != 0 && should_log_audio_server())
        dbgln("cid={}: create_audio_output_session_async: device selection not implemented, using default output", client_id());

    u64 session_id = s_next_audio_output_session_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);

    auto state = adopt_ref(*new AudioOutputSession::State);

    AudioOutputSession session {
        .id = session_id,
        .target_latency_ms = target_latency_ms,
        .sample_rate = 0,
        .channel_count = 0,
        .state = state,
    };

    m_audio_output_sessions.set(session_id, move(session));

    AudioOutputDevice& device = AudioOutputDevice::the();
    if (m_control_event_queue)
        device.ensure_started(*m_control_event_queue, target_latency_ms);

    device.when_ready([weak_self = make_weak_ptr<AudioServerConnection>(), session_id] {
        if (auto self = weak_self.strong_ref())
            self->finalize_audio_output_session_async(session_id);
    });

    return { session_id };
}

void AudioServerConnection::finalize_audio_output_session_async(u64 session_id)
{
    if (should_log_audio_server())
        dbgln("cid={}: finalize_audio_output_session_async(session_id={})", client_id(), session_id);

    auto session_or_error = m_audio_output_sessions.get(session_id);
    if (!session_or_error.has_value())
        return;
    auto& session = session_or_error.value();

    if (session.state->ring_ready.load(AK::MemoryOrder::memory_order_acquire))
        return;

    AudioOutputDevice& device = AudioOutputDevice::the();
    if (!device.has_sample_specification())
        return;

    u32 sample_rate = device.device_sample_rate_hz();
    u32 channel_count = device.device_channel_count();
    if (sample_rate == 0 || channel_count == 0) {
        warnln("cid={}: async audio output session={} invalid sample specification {} Hz, {} channels", client_id(), session_id, sample_rate, channel_count);
        if (is_open())
            async_audio_output_session_failed(session_id, ByteString("Invalid sample specification"));
        m_audio_output_sessions.remove(session_id);
        return;
    }

    auto ring_or_error = create_audio_output_ring(sample_rate, channel_count, session.target_latency_ms);
    if (ring_or_error.is_error()) {
        warnln("cid={}: async audio output session={} failed to create audio output ring: {}", client_id(), session_id, ring_or_error.error());
        if (is_open())
            async_audio_output_session_failed(session_id, ByteString("Failed to create audio ring"));
        m_audio_output_sessions.remove(session_id);
        return;
    }
    auto ring = ring_or_error.release_value();

    session.sample_rate = sample_rate;
    session.channel_count = channel_count;

    session.state->ring = ring;
    session.state->bytes_per_frame = static_cast<size_t>(channel_count) * sizeof(float);
    session.state->ring_ready.store(true, AK::MemoryOrder::memory_order_release);

    device.register_producer(session_id, ring, session.state->bytes_per_frame);
    device.set_producer_muted(session_id, m_is_muted);

    if (should_log_audio_server())
        dbgln("cid={}: async audio output session {} ready ({} Hz, {} channels)", client_id(), session_id, sample_rate, channel_count);

    if (is_open())
        async_audio_output_session_ready(session_id, sample_rate, channel_count, ring.anonymous_buffer());
}

void AudioServerConnection::destroy_audio_output_session(u64 session_id)
{
    if (should_log_audio_server())
        dbgln("cid={}: destroy_audio_output_session(session_id={})", client_id(), session_id);

    if (auto session = m_audio_output_sessions.get(session_id); session.has_value()) {
        if (session->state->ring_ready.load(AK::MemoryOrder::memory_order_acquire))
            AudioOutputDevice::the().unregister_producer(session_id);
    }
    m_audio_output_sessions.remove(session_id);
}

ErrorOr<IPC::File> AudioServerConnection::connect_new_client()
{
    int socket_fds[2] {};
    if (auto err = Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, socket_fds); err.is_error())
        return err.release_error();

    auto client_socket_or_error = Core::LocalSocket::adopt_fd(socket_fds[0]);
    if (client_socket_or_error.is_error()) {
        (void)Core::System::close(socket_fds[0]);
        (void)Core::System::close(socket_fds[1]);
        return client_socket_or_error.release_error();
    }

    auto client_socket = client_socket_or_error.release_value();
    auto transport = adopt_own(*new IPC::TransportSocket(move(client_socket)));
    (void)adopt_ref(*new AudioServerConnection(move(transport)));

    return IPC::File::adopt_fd(socket_fds[1]);
}

Messages::AudioServerServer::ConnectNewClientsResponse AudioServerConnection::connect_new_clients(size_t count)
{
    Vector<IPC::File> files;
    files.ensure_capacity(count);

    for (size_t i = 0; i < count; ++i) {
        auto file_or_error = connect_new_client();
        if (file_or_error.is_error()) {
            warnln("cid={}: failed to connect new client: {}", client_id(), file_or_error.error());
            return Vector<IPC::File> {};
        }
        files.unchecked_append(file_or_error.release_value());
    }

    return files;
}

}
