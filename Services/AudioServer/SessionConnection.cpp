/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AudioServer/Debug.h>
#include <AudioServer/InputStream.h>
#include <AudioServer/OutputDriver.h>
#include <AudioServer/Server.h>
#include <AudioServer/SessionConnection.h>
#include <LibAudioServer/SharedCircularBuffer.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibCore/ThreadEventQueue.h>
#include <bit>

namespace AudioServer {

using namespace Messages::ToAudioServerFromSessionClient;

SessionConnection::SessionConnection(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionFromClient<ToSessionClientFromAudioServerEndpoint, ToAudioServerFromSessionClientEndpoint>(*this, move(transport), Server::the().allocate_session_client_id())
{
    m_control_event_queue = &Core::ThreadEventQueue::current();

    Server::the().register_session_connection(*this);
}

SessionConnection::~SessionConnection()
{
    for (auto const& it : m_output_sinks) {
        auto const& session = it.value;
        if (session.state->ring_ready.load(AK::MemoryOrder::memory_order_acquire))
            Server::the().unregister_output_producer(session.device_handle, session.id);
    }
    m_output_sinks.clear();
    m_input_streams.clear();
}

void SessionConnection::die()
{
    verify_thread_affinity();
    auto id = client_id();

    if (should_log_audio_server())
        dbgln("cid={}: SessionConnection::die()", id);

    Server::the().unregister_session_connection(id);
    Server::the().release_session_client_id(id);
}

InitTransportResponse SessionConnection::init_transport([[maybe_unused]] int peer_pid)
{
    verify_thread_affinity();
    return { Core::System::getpid() };
}

static ErrorOr<AudioServer::SharedCircularBuffer> create_output_ring(u32 sample_rate, u32 channel_count, u32 target_latency_ms)
{
    if (sample_rate == 0 || channel_count == 0)
        return Error::from_string_literal("invalid sample specification");

    constexpr size_t minimum_callback_frames = 128;
    size_t bytes_per_frame = channel_count * sizeof(float);
    size_t minimum_callback_bytes = minimum_callback_frames * bytes_per_frame;
    size_t desired_latency_frames = static_cast<size_t>((static_cast<double>(sample_rate) * static_cast<double>(target_latency_ms)) / 1000.0);
    size_t desired_latency_bytes = max(desired_latency_frames * bytes_per_frame, minimum_callback_bytes * 2);

    size_t ring_capacity_bytes = std::bit_ceil(desired_latency_bytes);
    ring_capacity_bytes = max(ring_capacity_bytes, minimum_callback_bytes * 8);

    return AudioServer::SharedCircularBuffer::create(ring_capacity_bytes);
}

static Optional<DeviceInfo> resolve_output_device_for_session(DeviceHandle device_handle)
{
    if (device_handle != 0)
        return Server::the().get_device(device_handle);

    auto devices = Server::the().enumerate_devices();
    for (auto const& device : devices) {
        if (device.type == DeviceInfo::Type::Output && device.is_default)
            return device;
    }

    for (auto const& device : devices) {
        if (device.type == DeviceInfo::Type::Output)
            return device;
    }

    return {};
}

void SessionConnection::get_devices(u64 request_token)
{
    verify_thread_affinity();
    auto devices = Server::the().enumerate_devices();

    async_did_get_devices(request_token, move(devices));
}

void SessionConnection::create_input_stream(u64 request_token, ByteString grant_id, DeviceHandle device_handle, u64 capacity_frames)
{
    verify_thread_affinity();
    if (!Server::the().is_grant_active(grant_id)) {
        warnln("cid={}: create_input_stream denied (inactive or mismatched grant)", client_id());
        async_request_error(request_token, ByteString("Audio input grant is inactive or invalid"));
        return;
    }

    if (!Server::the().can_grant_use_mic(grant_id)) {
        warnln("cid={}: create_input_stream denied (grant has no microphone permission)", client_id());
        async_request_error(request_token, ByteString("Audio input grant does not allow microphone access"));
        return;
    }

    if (device_handle == 0) {
        async_request_error(request_token, ByteString("Invalid input device handle"));
        return;
    }

    auto device_info = Server::the().get_device(device_handle);
    if (!device_info.has_value() || device_info->type != DeviceInfo::Type::Input) {
        async_request_error(request_token, ByteString("Unknown audio input device handle"));
        return;
    }

    u32 sample_rate_hz = device_info->sample_rate_hz;
    u32 channel_count = device_info->channel_count;

    if (should_log_audio_server())
        dbgln("cid={}: create_input_stream(device_handle={}, sample_rate_hz={}, channel_count={}, capacity_frames={})", client_id(), device_handle, sample_rate_hz, channel_count, capacity_frames);

    if (sample_rate_hz == 0)
        sample_rate_hz = 48000;
    if (channel_count == 0)
        channel_count = 1;
    if (capacity_frames == 0)
        capacity_frames = 4096;

    auto stream_or_error = create_platform_input_stream(device_handle, sample_rate_hz, channel_count, capacity_frames);
    if (stream_or_error.is_error()) {
        async_request_error(request_token, ByteString::formatted("Failed to create input stream: {}", stream_or_error.error()));
        return;
    }

    auto stream = stream_or_error.release_value();
    u64 stream_id = Server::the().allocate_input_stream_id();
    stream->set_stream_id(stream_id);

    auto descriptor_or_error = stream->descriptor_for_ipc();
    if (descriptor_or_error.is_error()) {
        async_request_error(request_token, ByteString::formatted("Failed to prepare input stream descriptor: {}", descriptor_or_error.error()));
        return;
    }

    auto descriptor = descriptor_or_error.release_value();

    InputSource stream_session;
    stream_session.id = descriptor.stream_id;
    stream_session.device_handle = device_handle;
    stream_session.grant_id = move(grant_id);
    stream_session.started = true;
    stream_session.stream = move(stream);
    m_input_streams.set(stream_session.id, move(stream_session));

    async_did_create_input_stream(request_token, descriptor);
}

void SessionConnection::start_input_stream(u64 request_token, u64 stream_id)
{
    verify_thread_affinity();
    auto stream = m_input_streams.get(stream_id);
    if (!stream.has_value()) {
        async_request_error(request_token, ByteString("Unknown audio input stream id"));
        return;
    }

    if (!Server::the().is_grant_active(stream->grant_id)) {
        async_request_error(request_token, ByteString("Audio input grant is inactive or invalid"));
        return;
    }

    stream->started = true;
    async_did_start_input_stream(request_token);
}

void SessionConnection::stop_input_stream(u64 request_token, u64 stream_id)
{
    verify_thread_affinity();
    auto stream = m_input_streams.get(stream_id);
    if (!stream.has_value()) {
        async_request_error(request_token, ByteString("Unknown audio input stream id"));
        return;
    }

    destroy_input_stream_internal(stream_id);
    async_did_stop_input_stream(request_token);
}

void SessionConnection::destroy_input_stream(u64 request_token, u64 stream_id)
{
    verify_thread_affinity();
    if (should_log_audio_server())
        dbgln("cid={}: destroy_input_stream(stream_id={})", client_id(), stream_id);

    if (!m_input_streams.contains(stream_id)) {
        async_request_error(request_token, ByteString("Unknown audio input stream id"));
        return;
    }

    destroy_input_stream_internal(stream_id);
    async_did_destroy_input_stream(request_token);
}

void SessionConnection::set_output_sink_volume(u64 request_token, u64 session_id, double gain)
{
    verify_thread_affinity();
    auto session = m_output_sinks.get(session_id);
    if (!session.has_value()) {
        async_request_error(request_token, ByteString("Unknown audio output session id"));
        return;
    }

    float clamped_gain = static_cast<float>(clamp(gain, 0.0, 1.0));
    session->gain = clamped_gain;

    async_did_set_output_sink_volume(request_token);
}

void SessionConnection::create_session(u64 request_token, ByteString grant_id, u32 target_latency_ms, DeviceHandle device_handle)
{
    verify_thread_affinity();
    (void)grant_id;

    if (should_log_audio_server())
        dbgln("cid={}: create_output_sink(target_latency_ms={}, device_handle={})", client_id(), target_latency_ms, device_handle);

    if (device_handle != 0 && !Server::the().get_device(device_handle).has_value()) {
        warnln("cid={}: create_output_sink_async: unknown device_handle={}", client_id(), device_handle);
        async_request_error(request_token, ByteString("Unknown audio output device handle"));
        return;
    }

    u64 session_id = Server::the().allocate_output_sink_id();

    auto state = adopt_ref(*new OutputSink::State);

    OutputSink session {
        .id = session_id,
        .device_handle = device_handle,
        .target_latency_ms = target_latency_ms,
        .sample_rate = 0,
        .channel_count = 0,
        .gain = 1.0f,
        .state = state,
    };

    m_output_sinks.set(session_id, move(session));

    if (m_control_event_queue)
        Server::the().ensure_output_device_started(device_handle, *m_control_event_queue, target_latency_ms);

    Server::the().when_output_device_ready(device_handle, [weak_self = make_weak_ptr<SessionConnection>(), session_id] {
        if (auto self = weak_self.strong_ref())
            self->finalize_output_sink(session_id);
    });

    async_did_create_session(request_token, session_id);
}

void SessionConnection::finalize_output_sink(u64 session_id)
{
    verify_thread_affinity();
    if (should_log_audio_server())
        dbgln("cid={}: finalize_output_sink_async(session_id={})", client_id(), session_id);

    auto session_or_error = m_output_sinks.get(session_id);
    if (!session_or_error.has_value())
        return;
    auto& session = session_or_error.value();

    if (session.state->ring_ready.load(AK::MemoryOrder::memory_order_acquire))
        return;

    auto maybe_device = resolve_output_device_for_session(session.device_handle);

    if (!maybe_device.has_value()) {
        warnln("cid={}: async audio output session={} unknown output device handle {}", client_id(), session_id, session.device_handle);
        fail_output_sink(session_id, ByteString("Unknown audio output device"));
        return;
    }

    auto device_info = maybe_device.release_value();
    u32 sample_rate = device_info.sample_rate_hz;
    u32 channel_count = device_info.channel_count;
    Audio::ChannelMap channel_layout = device_info.channel_layout;
    if (sample_rate == 0 || channel_count == 0) {
        warnln("cid={}: async audio output session={} invalid sample specification {} Hz, {} channels", client_id(), session_id, sample_rate, channel_count);
        fail_output_sink(session_id, ByteString("Invalid sample specification"));
        return;
    }
    if (channel_layout.channel_count() != channel_count) {
        warnln("cid={}: async audio output session={} invalid channel layout (layout_count={}, channel_count={})", client_id(), session_id, channel_layout.channel_count(), channel_count);
        fail_output_sink(session_id, ByteString("Invalid channel layout"));
        return;
    }

    auto ring_or_error = create_output_ring(sample_rate, channel_count, session.target_latency_ms);
    if (ring_or_error.is_error()) {
        warnln("cid={}: async audio output session={} failed to create audio output ring: {}", client_id(), session_id, ring_or_error.error());
        fail_output_sink(session_id, ByteString("Failed to create audio ring"));
        return;
    }
    auto ring = ring_or_error.release_value();

    auto timing_buffer_or_error = OutputStream::create_timing_buffer();
    if (timing_buffer_or_error.is_error()) {
        warnln("cid={}: async audio output session={} failed to create audio timing buffer: {}", client_id(), session_id, timing_buffer_or_error.error());
        fail_output_sink(session_id, ByteString("Failed to create audio timing buffer"));
        return;
    }
    auto timing_buffer = timing_buffer_or_error.release_value();

    session.sample_rate = sample_rate;
    session.channel_count = channel_count;

    session.state->ring = ring;
    session.state->timing_buffer = timing_buffer;
    session.state->bytes_per_frame = static_cast<size_t>(channel_count) * sizeof(float);
    session.state->ring_ready.store(true, AK::MemoryOrder::memory_order_release);

    Server::the().register_output_producer(session.device_handle, session_id, ring, timing_buffer, session.state->bytes_per_frame);

    if (should_log_audio_server())
        dbgln("cid={}: async audio output session {} ready ({} Hz, {} channels)", client_id(), session_id, sample_rate, channel_count);

    if (is_open()) {
        async_output_sink_ready(OutputSinkTransport {
            .session_id = session_id,
            .sample_rate = sample_rate,
            .channel_count = channel_count,
            .channel_layout = channel_layout,
            .sample_ring_buffer = ring,
            .timing_buffer = move(timing_buffer),
        });
    }
}

void SessionConnection::fail_output_sink(u64 session_id, ByteString const& error)
{
    verify_thread_affinity();
    if (is_open())
        async_output_sink_failed(session_id, error);
    m_output_sinks.remove(session_id);
}

void SessionConnection::destroy_output_sink_internal(u64 session_id)
{
    verify_thread_affinity();
    if (should_log_audio_server())
        dbgln("cid={}: destroy_output_sink(session_id={})", client_id(), session_id);

    if (auto session = m_output_sinks.get(session_id); session.has_value()) {
        if (session->state->ring_ready.load(AK::MemoryOrder::memory_order_acquire))
            Server::the().unregister_output_producer(session->device_handle, session_id);
    }
    m_output_sinks.remove(session_id);
}

void SessionConnection::destroy_session(u64 request_token, u64 session_id)
{
    verify_thread_affinity();
    if (!m_output_sinks.contains(session_id)) {
        async_request_error(request_token, ByteString("Unknown audio output session id"));
        return;
    }

    destroy_output_sink_internal(session_id);
    async_did_destroy_session(request_token);
}

void SessionConnection::destroy_input_stream_internal(u64 stream_id)
{
    verify_thread_affinity();
    m_input_streams.remove(stream_id);
}

ErrorOr<IPC::File> SessionConnection::connect_new_client_for_broker()
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
    (void)adopt_ref(*new SessionConnection(move(transport)));

    return IPC::File::adopt_fd(socket_fds[1]);
}

void SessionConnection::stop_all_streams_for_grant_revocation(ByteString const& grant_id)
{
    verify_thread_affinity();
    Vector<u64> input_streams_to_remove;
    for (auto const& it : m_input_streams) {
        if (it.value.grant_id != grant_id)
            continue;
        input_streams_to_remove.append(it.key);
    }

    for (u64 stream_id : input_streams_to_remove)
        destroy_input_stream_internal(stream_id);
}

void SessionConnection::notify_devices_changed()
{
    verify_thread_affinity();
    if (is_open())
        async_notify_devices_changed();
}

}
