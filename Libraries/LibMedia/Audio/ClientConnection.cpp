/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibCore/System.h>
#include <LibMedia/Audio/ClientConnection.h>
#include <LibMedia/Audio/RemotePlaybackStream.h>

namespace Audio {

// The ring only has to cover the pump's cadence; the device's own buffer spends the rest of a stream's budget.
static constexpr u32 MINIMUM_RING_LATENCY_MS = 30;

static NeverDestroyed<ClientConnection::TransportFactory> s_transport_factory;
static NeverDestroyed<RefPtr<ClientConnection>> s_connection;

void ClientConnection::set_transport_factory(TransportFactory factory)
{
    *s_transport_factory = move(factory);
}

bool ClientConnection::has_transport_factory()
{
    return !!*s_transport_factory;
}

ErrorOr<NonnullRefPtr<ClientConnection>> ClientConnection::acquire()
{
    if (*s_connection)
        return NonnullRefPtr(**s_connection);
    if (!*s_transport_factory)
        return Error::from_string_literal("No audio server transport factory is installed");

    auto transport = TRY((*s_transport_factory)());
    auto connection = adopt_ref(*new ClientConnection(move(transport)));
#ifdef AK_OS_WINDOWS
    // A transport that already knows its peer, as one paired within a process does, needs no introduction.
    if (connection->transport().peer_pid() == -1) {
        auto response = connection->send_sync<Messages::AudioServer::InitTransport>(Core::System::getpid());
        connection->transport().set_peer_pid(response->peer_pid());
    }
#endif
    *s_connection = connection;
    return connection;
}

ClientConnection::ClientConnection(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<AudioClientEndpoint, AudioServerEndpoint>(*this, move(transport))
    , m_event_loop(Core::EventLoop::current())
{
    m_pump_thread = Threading::Thread::construct("Audio Pump"sv, [this]() -> intptr_t {
        pump_thread_main();
        return 0;
    });
    m_pump_thread->set_qos(Core::Platform::ThreadQoS::UserInteractive);
    m_pump_thread->start();
}

ClientConnection::~ClientConnection()
{
    {
        MutexLocker locker(m_pump_mutex);
        m_pump_should_exit = true;
        m_pump_condition.broadcast();
    }
    (void)m_pump_thread->join();
}

void ClientConnection::die()
{
    // NB: Forgetting this connection below can drop the last reference to it.
    NonnullRefPtr keep_alive_until_death_is_handled = *this;
    m_is_dead = true;
    if (*s_connection == this)
        *s_connection = nullptr;

    auto pending_streams = move(m_pending_streams);
    for (auto& [stream_id, pending] : pending_streams)
        pending.promise->reject(Error::from_string_literal("Audio server connection died before the stream was created"));

    Vector<NonnullRefPtr<RemotePlaybackStream>> live_streams;
    {
        MutexLocker locker(m_pump_mutex);
        for (auto* stream : m_streams) {
            if (stream->try_ref())
                live_streams.append(adopt_ref(*stream));
        }
    }
    for (auto& stream : live_streams)
        stream->connection_lost();

    auto pending_requests = move(m_pending_requests);
    for (auto& [request_id, request] : pending_requests)
        request.on_complete(find_live_stream(request.stream_id).ptr());

    // Whatever the server knew went with it; the watcher hears so once.
    if (auto on_device_list = move(m_on_device_list))
        on_device_list(Error::from_string_literal("Audio server connection died"));
}

void ClientConnection::watch_devices(Media::AudioDeviceListCallback on_device_list)
{
    VERIFY(!m_on_device_list);
    if (m_is_dead) {
        on_device_list(Error::from_string_literal("Audio server connection has died"));
        return;
    }
    m_on_device_list = move(on_device_list);
    async_watch_devices();
}

void ClientConnection::devices_changed(Vector<Media::AudioDeviceInfo> inputs, Vector<Media::AudioDeviceInfo> outputs)
{
    if (m_on_device_list)
        m_on_device_list(Media::AudioDeviceEnumeration { .inputs = move(inputs), .outputs = move(outputs) });
}

RefPtr<RemotePlaybackStream> ClientConnection::find_live_stream(u64 stream_id)
{
    MutexLocker locker(m_pump_mutex);
    for (auto* stream : m_streams) {
        if (stream->stream_id() == stream_id && stream->try_ref())
            return adopt_ref(*stream);
    }
    return nullptr;
}

NonnullRefPtr<PlaybackStream::CreatePromise> ClientConnection::create_stream(OutputState initial_state, u32 target_latency_ms, PlaybackStream::AudioDataRequestCallback callback)
{
    auto promise = PlaybackStream::CreatePromise::construct();
    if (m_is_dead) {
        promise->reject(Error::from_string_literal("Audio server connection has died"));
        return promise;
    }
    auto stream_id = m_next_stream_id++;
    m_pending_streams.set(stream_id, PendingStream { .promise = promise, .callback = move(callback), .initial_state = initial_state, .target_latency_ms = target_latency_ms });
    async_create_stream(stream_id);
    return promise;
}

void ClientConnection::stream_created(u64 stream_id, SampleSpecification sample_specification, u32 device_latency_ms)
{
    auto pending = m_pending_streams.take(stream_id);
    if (!pending.has_value()) {
        async_destroy_stream(stream_id);
        return;
    }
    if (!sample_specification.is_valid()) {
        async_destroy_stream(stream_id);
        pending->promise->reject(Error::from_string_literal("Audio server reported an invalid device format"));
        return;
    }
    u32 ring_latency_ms = MINIMUM_RING_LATENCY_MS;
    if (pending->target_latency_ms > device_latency_ms + MINIMUM_RING_LATENCY_MS)
        ring_latency_ms = pending->target_latency_ms - device_latency_ms;
    {
        MutexLocker locker(m_pump_mutex);
        m_pump_period = min(m_pump_period, AK::Duration::from_milliseconds(max<u32>(1, ring_latency_ms / 4)));
    }
    auto stream = adopt_ref(*new RemotePlaybackStream(*this, stream_id, sample_specification, ring_latency_ms, pending->initial_state, move(pending->callback)));
    pending->promise->resolve(stream);
}

void ClientConnection::stream_creation_failed(u64 stream_id)
{
    auto pending = m_pending_streams.take(stream_id);
    if (!pending.has_value())
        return;
    pending->promise->reject(Error::from_string_literal("Audio server could not open an output device"));
}

void ClientConnection::stream_request_completed(u64 stream_id, u64 request_id)
{
    auto request = m_pending_requests.take(request_id);
    if (!request.has_value() || request->stream_id != stream_id)
        return;
    request->on_complete(find_live_stream(stream_id).ptr());
}

void ClientConnection::send_request(Badge<RemotePlaybackStream>, u64 stream_id, StreamRequest request, RequestCompletion on_complete)
{
    if (m_is_dead) {
        on_complete(find_live_stream(stream_id).ptr());
        return;
    }
    auto request_id = m_next_request_id++;
    m_pending_requests.set(request_id, PendingRequest { .stream_id = stream_id, .on_complete = move(on_complete) });
    switch (request) {
    case StreamRequest::Resume:
        async_resume_stream(stream_id, request_id);
        break;
    case StreamRequest::Drain:
        async_drain_stream(stream_id, request_id);
        break;
    case StreamRequest::Discard:
        async_discard_stream(stream_id, request_id);
        break;
    }
}

void ClientConnection::stream_destroyed(Badge<RemotePlaybackStream>, u64 stream_id)
{
    if (m_is_dead)
        return;
    async_destroy_stream(stream_id);
}

void ClientConnection::register_stream(Badge<RemotePlaybackStream>, RemotePlaybackStream& stream)
{
    MutexLocker locker(m_pump_mutex);
    m_streams.append(&stream);
    m_pump_woken = true;
    m_pump_condition.signal();
}

void ClientConnection::unregister_stream(Badge<RemotePlaybackStream>, RemotePlaybackStream& stream)
{
    MutexLocker locker(m_pump_mutex);
    m_streams.remove_first_matching([&](auto* candidate) { return candidate == &stream; });
}

void ClientConnection::wake_pump()
{
    MutexLocker locker(m_pump_mutex);
    m_pump_woken = true;
    m_pump_condition.signal();
}

void ClientConnection::pump_thread_main()
{
    Vector<float> scratch;
    Vector<NonnullRefPtr<RemotePlaybackStream>> streams;
    MutexLocker locker(m_pump_mutex);
    while (!m_pump_should_exit) {
        streams.clear_with_capacity();
        for (auto* stream : m_streams) {
            if (stream->try_ref())
                streams.append(adopt_ref(*stream));
        }
        m_pump_woken = false;

        if (streams.is_empty()) {
            m_pump_condition.wait();
            continue;
        }

        auto period = m_pump_period;
        locker.unlock();
        for (auto& stream : streams)
            stream->fill(scratch);
        // Drop the references before sleeping, so a stream's last reference does not idle here.
        streams.clear_with_capacity();
        locker.lock();

        if (!m_pump_woken && !m_pump_should_exit)
            (void)m_pump_condition.wait_for(period);
    }
}

}
