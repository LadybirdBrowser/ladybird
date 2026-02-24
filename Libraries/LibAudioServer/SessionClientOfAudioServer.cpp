/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibAudioServer/SessionClientOfAudioServer.h>
#include <LibThreading/Mutex.h>

namespace AudioServer {

using namespace Messages::ToAudioServerFromSessionClient;

static Threading::Mutex s_default_client_mutex;
static RefPtr<SessionClientOfAudioServer> s_default_client;

void SessionClientOfAudioServer::set_default_client(RefPtr<SessionClientOfAudioServer> client)
{
    Threading::MutexLocker locker { s_default_client_mutex };
    s_default_client = move(client);
}

RefPtr<SessionClientOfAudioServer> SessionClientOfAudioServer::default_client()
{
    Threading::MutexLocker locker { s_default_client_mutex };
    return s_default_client;
}

SessionClientOfAudioServer::SessionClientOfAudioServer(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<ToSessionClientFromAudioServerEndpoint, ToAudioServerFromSessionClientEndpoint>(*this, move(transport))
{
}

u64 SessionClientOfAudioServer::add_output_sink(OutputSinkReady on_ready, OutputSinkFailed on_failed)
{
    verify_thread_affinity();
    u64 sink_id = m_next_output_sink_id++;
    m_create_sink_callbacks.set(sink_id, { .on_ready = move(on_ready), .on_failed = move(on_failed) });
    return sink_id;
}

void SessionClientOfAudioServer::remove_output_sink(u64 sink_id)
{
    verify_thread_affinity();
    m_create_sink_callbacks.remove(sink_id);
}

void SessionClientOfAudioServer::complete_pending_request_error(u64 request_token, ByteString error)
{
    verify_thread_affinity();
    m_pending_request_results.remove(request_token);

    auto callback = m_pending_request_errors.take(request_token);
    if (callback.has_value())
        callback.value()(move(error));
}

u64 SessionClientOfAudioServer::next_request_token()
{
    verify_thread_affinity();
    return m_next_request_token++;
}

void SessionClientOfAudioServer::die()
{
    verify_thread_affinity();
    for (auto request_token : m_pending_request_results.keys())
        complete_pending_request_error(request_token, ByteString("Audio session connection died"));

    [[maybe_unused]] auto self = NonnullRefPtr(*this);

    auto death_callback = move(on_death);
    on_death = {};
    if (death_callback)
        death_callback();
}

ErrorOr<u64> SessionClientOfAudioServer::set_output_sink_volume(u64 session_id, double volume, Function<void()> on_success, RequestErrorHandler on_error)
{
    verify_thread_affinity();
    u64 request_token = next_request_token();
    store_pending_request(request_token, [on_success = move(on_success)]() mutable { if (on_success) on_success(); }, move(on_error));

    if (post_message(SetOutputSinkVolume(request_token, session_id, volume)).is_error()) {
        complete_pending_request_error(request_token, ByteString("AudioServerClient: set output session gain IPC failed"));
        return Error::from_string_literal("AudioServerClient: set output session gain IPC failed");
    }

    return request_token;
}

ErrorOr<u64> SessionClientOfAudioServer::create_session(u32 target_latency_ms, Function<void(u64)> on_success, RequestErrorHandler on_error, AudioServer::DeviceHandle device_handle)
{
    verify_thread_affinity();
    u64 request_token = next_request_token();
    store_pending_request(request_token, [this, request_token, on_success = move(on_success)](u64 created_session_id) mutable {
        if (created_session_id == 0) {
            complete_pending_request_error(request_token, ByteString("AudioServerClient: server returned invalid audio session id"));
            return;
        }
        if (on_success)
            on_success(created_session_id); }, move(on_error));

    if (post_message(CreateSession(request_token, m_grant_id, target_latency_ms, device_handle)).is_error()) {
        complete_pending_request_error(request_token, ByteString("AudioServerClient: create output session IPC failed"));
        return Error::from_string_literal("AudioServerClient: create output session IPC failed");
    }

    return request_token;
}

void SessionClientOfAudioServer::output_sink_ready(AudioServer::OutputSinkTransport output_sink_transport)
{
    verify_thread_affinity();
    auto session_id = output_sink_transport.session_id;
    auto fail = [&](char const* message) { if (on_output_sink_failed) on_output_sink_failed(session_id, ByteString(message)); };
    if (session_id == 0 || output_sink_transport.sample_rate == 0 || output_sink_transport.channel_count == 0) {
        fail("Server returned invalid audio session format");
        return;
    }
    if (output_sink_transport.channel_layout.channel_count() != output_sink_transport.channel_count) {
        fail("Server returned invalid audio channel layout");
        return;
    }
    if (!output_sink_transport.sample_ring_buffer.is_valid()) {
        fail("Server returned invalid audio ring buffer");
        return;
    }
    if (!output_sink_transport.timing_buffer.is_valid()) {
        fail("Server returned invalid audio timing buffer");
        return;
    }

    auto timing_or_error = TimingReader::attach(move(output_sink_transport.timing_buffer));
    if (timing_or_error.is_error()) {
        fail("Failed to attach audio timing buffer");
        return;
    }
    OutputSink output_sink {
        .session_id = session_id,
        .sample_rate = output_sink_transport.sample_rate,
        .channel_count = output_sink_transport.channel_count,
        .channel_layout = output_sink_transport.channel_layout,
        .ring = move(output_sink_transport.sample_ring_buffer),
        .timing = timing_or_error.release_value(),
    };
    bool handled_by_sink = false;
    for (auto sink_id : m_create_sink_callbacks.keys()) {
        auto it = m_create_sink_callbacks.find(sink_id);
        if (it == m_create_sink_callbacks.end() || !it->value.on_ready)
            continue;
        if (it->value.on_ready(output_sink)) {
            handled_by_sink = true;
            break;
        }
    }

    if (!handled_by_sink && on_output_sink_ready)
        on_output_sink_ready(move(output_sink));
}

void SessionClientOfAudioServer::output_sink_failed(u64 session_id, ByteString error)
{
    verify_thread_affinity();
    for (auto sink_id : m_create_sink_callbacks.keys()) {
        auto it = m_create_sink_callbacks.find(sink_id);
        if (it != m_create_sink_callbacks.end() && it->value.on_failed)
            it->value.on_failed(session_id, error);
    }

    if (on_output_sink_failed)
        on_output_sink_failed(session_id, move(error));
}

void SessionClientOfAudioServer::notify_devices_changed()
{
    verify_thread_affinity();
    if (on_devices_changed)
        on_devices_changed();
}

ErrorOr<u64> SessionClientOfAudioServer::destroy_session(u64 session_id, Function<void()> on_success, RequestErrorHandler on_error)
{
    verify_thread_affinity();
    u64 request_token = next_request_token();
    store_pending_request(request_token, [on_success = move(on_success)]() mutable { if (on_success) on_success(); }, move(on_error));

    if (post_message(DestroySession(request_token, session_id)).is_error()) {
        complete_pending_request_error(request_token, ByteString("AudioServerClient: destroy output session IPC failed"));
        return Error::from_string_literal("AudioServerClient: destroy output session IPC failed");
    }

    return request_token;
}

ErrorOr<u64> SessionClientOfAudioServer::get_devices(Function<void(Vector<AudioServer::DeviceInfo>)> on_success, RequestErrorHandler on_error)
{
    verify_thread_affinity();
    u64 request_token = next_request_token();
    store_pending_request(request_token, [on_success = move(on_success)](Vector<AudioServer::DeviceInfo> devices) mutable { if (on_success) on_success(move(devices)); }, move(on_error));

    if (post_message(GetDevices(request_token)).is_error()) {
        complete_pending_request_error(request_token, ByteString("AudioServerClient: get devices IPC failed"));
        return Error::from_string_literal("AudioServerClient: get devices IPC failed");
    }

    return request_token;
}

ErrorOr<u64> SessionClientOfAudioServer::create_input_stream(AudioServer::DeviceHandle device_handle, u64 capacity_frames, Function<void(AudioServer::InputStreamDescriptor)> on_success, RequestErrorHandler on_error)
{
    verify_thread_affinity();
    if (m_grant_id.is_empty())
        return Error::from_string_literal("AudioServerClient: no grant id configured");

    u64 request_token = next_request_token();
    store_pending_request(request_token, [this, request_token, on_success = move(on_success)](AudioServer::InputStreamDescriptor created_descriptor) mutable {
        if (created_descriptor.stream_id == 0 || !created_descriptor.shared_memory.is_valid()) {
            complete_pending_request_error(request_token, ByteString("AudioServerClient: server returned invalid audio input stream descriptor"));
            return;
        }
        if (on_success)
            on_success(move(created_descriptor)); }, move(on_error));

    if (post_message(CreateInputStream(request_token, m_grant_id, device_handle, capacity_frames)).is_error()) {
        complete_pending_request_error(request_token, ByteString("AudioServerClient: create input stream IPC failed"));
        return Error::from_string_literal("AudioServerClient: create input stream IPC failed");
    }

    return request_token;
}

ErrorOr<u64> SessionClientOfAudioServer::start_input_stream(u64 stream_id, Function<void()> on_success, RequestErrorHandler on_error)
{
    verify_thread_affinity();
    u64 request_token = next_request_token();
    store_pending_request(request_token, [on_success = move(on_success)]() mutable { if (on_success) on_success(); }, move(on_error));

    if (post_message(StartInputStream(request_token, stream_id)).is_error()) {
        complete_pending_request_error(request_token, ByteString("AudioServerClient: start input stream IPC failed"));
        return Error::from_string_literal("AudioServerClient: start input stream IPC failed");
    }

    return request_token;
}

ErrorOr<u64> SessionClientOfAudioServer::stop_input_stream(u64 stream_id, Function<void()> on_success, RequestErrorHandler on_error)
{
    verify_thread_affinity();
    u64 request_token = next_request_token();
    store_pending_request(request_token, [on_success = move(on_success)]() mutable { if (on_success) on_success(); }, move(on_error));

    if (post_message(StopInputStream(request_token, stream_id)).is_error()) {
        complete_pending_request_error(request_token, ByteString("AudioServerClient: stop input stream IPC failed"));
        return Error::from_string_literal("AudioServerClient: stop input stream IPC failed");
    }

    return request_token;
}

ErrorOr<u64> SessionClientOfAudioServer::destroy_input_stream(u64 stream_id, Function<void()> on_success, RequestErrorHandler on_error)
{
    verify_thread_affinity();
    if (stream_id == 0)
        return Error::from_string_literal("AudioServerClient: invalid audio input stream id");

    u64 request_token = next_request_token();
    store_pending_request(request_token, [on_success = move(on_success)]() mutable { if (on_success) on_success(); }, move(on_error));

    if (post_message(DestroyInputStream(request_token, stream_id)).is_error()) {
        complete_pending_request_error(request_token, ByteString("AudioServerClient: destroy input stream IPC failed"));
        return Error::from_string_literal("AudioServerClient: destroy input stream IPC failed");
    }

    return request_token;
}

void SessionClientOfAudioServer::did_get_devices(u64 request_token, Vector<AudioServer::DeviceInfo> devices)
{
    dispatch_pending_request<PendingDeviceInfosResult>(request_token, move(devices));
}

void SessionClientOfAudioServer::did_create_input_stream(u64 request_token, AudioServer::InputStreamDescriptor descriptor)
{
    dispatch_pending_request<PendingInputStreamDescriptorResult>(request_token, move(descriptor));
}

void SessionClientOfAudioServer::did_start_input_stream(u64 request_token)
{
    dispatch_pending_request<PendingVoidResult>(request_token);
}

void SessionClientOfAudioServer::did_stop_input_stream(u64 request_token)
{
    dispatch_pending_request<PendingVoidResult>(request_token);
}

void SessionClientOfAudioServer::did_destroy_input_stream(u64 request_token)
{
    dispatch_pending_request<PendingVoidResult>(request_token);
}

void SessionClientOfAudioServer::did_set_output_sink_volume(u64 request_token)
{
    dispatch_pending_request<PendingVoidResult>(request_token);
}

void SessionClientOfAudioServer::did_create_session(u64 request_token, u64 session_id)
{
    dispatch_pending_request<PendingU64Result>(request_token, session_id);
}

void SessionClientOfAudioServer::did_destroy_session(u64 request_token)
{
    dispatch_pending_request<PendingVoidResult>(request_token);
}

void SessionClientOfAudioServer::request_error(u64 request_token, ByteString error)
{
    verify_thread_affinity();
    complete_pending_request_error(request_token, move(error));
}

}
