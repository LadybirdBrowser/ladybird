/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibAudio/ClientConnection.h>
#include <LibAudio/RemotePlaybackStream.h>
#include <LibAudio/RemoteRecordStream.h>

namespace Audio {

// The ring only has to cover the pump's cadence; the device's own buffer spends the rest of a stream's budget.
static constexpr u32 MINIMUM_RING_LATENCY_MS = 30;
// The server captures in fragments of this length, so the pump need not look more often.
static constexpr AK::Duration CAPTURE_PUMP_PERIOD = AK::Duration::from_milliseconds(20);

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
    Vector<NonnullRefPtr<RemoteRecordStream>> live_record_streams;
    {
        MutexLocker locker(m_pump_mutex);
        for (auto* stream : m_streams) {
            if (stream->try_ref())
                live_streams.append(adopt_ref(*stream));
        }
        for (auto* stream : m_record_streams) {
            if (stream->try_ref())
                live_record_streams.append(adopt_ref(*stream));
        }
        m_capture_rings.clear();
    }
    for (auto& stream : live_streams)
        stream->connection_lost();

    auto capture_sources = move(m_capture_sources);
    for (auto& [stream_id, source] : capture_sources) {
        for (auto& pending : source.pending)
            pending.promise->reject(Error::from_string_literal("Audio server connection died before the capture stream was created"));
    }
    for (auto& stream : live_record_streams)
        stream->connection_lost();

    auto pending_requests = move(m_pending_requests);
    for (auto& [request_id, request] : pending_requests)
        request.on_complete(find_live_stream(request.stream_id).ptr());

    // Whatever the server knew went with it; the watcher hears so once.
    if (auto on_device_list = move(m_on_device_list))
        on_device_list(Error::from_string_literal("Audio server connection died"));
}

void ClientConnection::watch_devices(AudioDeviceListCallback on_device_list)
{
    VERIFY(!m_on_device_list);
    if (m_is_dead) {
        on_device_list(Error::from_string_literal("Audio server connection has died"));
        return;
    }
    m_on_device_list = move(on_device_list);
    async_watch_devices();
}

void ClientConnection::request_device_list()
{
    if (!m_is_dead)
        async_watch_devices();
}

void ClientConnection::devices_changed(Vector<AudioDeviceInfo> inputs, Vector<AudioDeviceInfo> outputs)
{
    if (m_on_device_list)
        m_on_device_list(AudioDeviceEnumeration { .inputs = move(inputs), .outputs = move(outputs) });
}

NonnullRefPtr<RecordStream::CreatePromise> ClientConnection::create_record_stream(StringView device_id, RecordStream::RecordCallback callback)
{
    auto promise = RecordStream::CreatePromise::construct();
    if (m_is_dead) {
        promise->reject(Error::from_string_literal("Audio server connection has died"));
        return promise;
    }

    for (auto& [stream_id, source] : m_capture_sources) {
        if (source.device_id != device_id)
            continue;
        if (!source.specification.has_value()) {
            source.pending.append(PendingRecordStream { .promise = promise, .callback = move(callback) });
            return promise;
        }
        source.stream_count++;
        promise->resolve(adopt_ref(*new RemoteRecordStream(*this, stream_id, source.specification.value(), move(callback))));
        return promise;
    }

    auto stream_id = m_next_stream_id++;
    CaptureSource source { .device_id = device_id, .specification = {}, .pending = {}, .stream_count = 0 };
    source.pending.append(PendingRecordStream { .promise = promise, .callback = move(callback) });
    m_capture_sources.set(stream_id, move(source));
    async_create_record_stream(stream_id, device_id);
    return promise;
}

void ClientConnection::record_stream_created(u64 stream_id, SharedAudioFrameRing ring)
{
    auto source = m_capture_sources.get(stream_id);
    if (!source.has_value()) {
        async_destroy_record_stream(stream_id);
        return;
    }
    // FIXME: Support channel layouts beyond mono and stereo.
    if (ring.channel_count() == 0 || ring.channel_count() > 2) {
        async_destroy_record_stream(stream_id);
        record_stream_creation_failed(stream_id);
        return;
    }
    SampleSpecification specification(ring.sample_rate(), ring.channel_count() == 1 ? ChannelMap::mono() : ChannelMap::stereo());
    source->specification = specification;
    {
        MutexLocker locker(m_pump_mutex);
        m_capture_rings.set(stream_id, move(ring));
        m_pump_period = min(m_pump_period, CAPTURE_PUMP_PERIOD);
        m_pump_woken = true;
        m_pump_condition.signal();
    }

    // Resolving may create further streams, so the source is not touched past this point.
    auto pending = move(source->pending);
    source->stream_count += pending.size();
    for (auto& pending_stream : pending)
        pending_stream.promise->resolve(adopt_ref(*new RemoteRecordStream(*this, stream_id, specification, move(pending_stream.callback))));
}

void ClientConnection::record_stream_creation_failed(u64 stream_id)
{
    auto source = m_capture_sources.take(stream_id);
    if (!source.has_value())
        return;
    for (auto& pending_stream : source->pending)
        pending_stream.promise->reject(Error::from_string_literal("Audio server could not open the capture device"));
}

void ClientConnection::record_stream_closed(u64 stream_id)
{
    // The source is forgotten first, so that a stream asked for from inside a callback asks the server afresh.
    auto source = m_capture_sources.take(stream_id);
    if (!source.has_value())
        return;
    Vector<NonnullRefPtr<RemoteRecordStream>> live_streams;
    {
        MutexLocker locker(m_pump_mutex);
        m_capture_rings.remove(stream_id);
        for (auto* stream : m_record_streams) {
            if (stream->stream_id() == stream_id && stream->try_ref())
                live_streams.append(adopt_ref(*stream));
        }
    }
    for (auto& pending_stream : source->pending)
        pending_stream.promise->reject(Error::from_string_literal("The tab may no longer capture audio"));
    for (auto& stream : live_streams)
        stream->capture_revoked();
}

void ClientConnection::record_stream_unsubscribed(Badge<RemoteRecordStream>, u64 stream_id)
{
    auto source = m_capture_sources.get(stream_id);
    if (!source.has_value())
        return;
    VERIFY(source->stream_count > 0);
    source->stream_count--;
    if (source->stream_count == 0 && source->pending.is_empty())
        forget_capture_source(stream_id);
}

void ClientConnection::forget_capture_source(u64 stream_id)
{
    m_capture_sources.remove(stream_id);
    {
        MutexLocker locker(m_pump_mutex);
        m_capture_rings.remove(stream_id);
    }
    if (!m_is_dead)
        async_destroy_record_stream(stream_id);
}

void ClientConnection::register_record_stream(Badge<RemoteRecordStream>, RemoteRecordStream& stream)
{
    MutexLocker locker(m_pump_mutex);
    m_record_streams.append(&stream);
    m_pump_woken = true;
    m_pump_condition.signal();
}

void ClientConnection::unregister_record_stream(Badge<RemoteRecordStream>, RemoteRecordStream& stream)
{
    MutexLocker locker(m_pump_mutex);
    m_record_streams.remove_first_matching([&](auto* candidate) { return candidate == &stream; });
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

NonnullRefPtr<PlaybackStream::CreatePromise> ClientConnection::create_stream(OutputState initial_state, u32 target_latency_ms, NonnullRefPtr<OutputSession> session, PlaybackStream::AudioDataRequestCallback callback)
{
    auto promise = PlaybackStream::CreatePromise::construct();
    if (m_is_dead) {
        promise->reject(Error::from_string_literal("Audio server connection has died"));
        return promise;
    }
    session->declare_on({}, *this);
    auto stream_id = m_next_stream_id++;
    auto session_id = session->id();
    m_pending_streams.set(stream_id, PendingStream { .promise = promise, .session = move(session), .callback = move(callback), .initial_state = initial_state, .target_latency_ms = target_latency_ms });
    async_create_stream(stream_id, session_id);
    return promise;
}

void ClientConnection::session_rebound(Badge<OutputSession>, u64 session_id, PlaybackGroupId group)
{
    if (m_is_dead)
        return;
    async_rebind_session(session_id, group);
}

void ClientConnection::session_destroyed(Badge<OutputSession>, u64 session_id)
{
    if (m_is_dead)
        return;
    async_destroy_session(session_id);
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
    auto stream = adopt_ref(*new RemotePlaybackStream(*this, stream_id, move(pending->session), sample_specification, ring_latency_ms, pending->initial_state, move(pending->callback)));
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
    struct CaptureRing {
        u64 stream_id { 0 };
        SharedAudioFrameRing ring;
    };

    Vector<float> scratch;
    Vector<NonnullRefPtr<RemotePlaybackStream>> streams;
    Vector<NonnullRefPtr<RemoteRecordStream>> record_streams;
    Vector<CaptureRing> capture_rings;
    MutexLocker locker(m_pump_mutex);
    while (!m_pump_should_exit) {
        streams.clear_with_capacity();
        record_streams.clear_with_capacity();
        capture_rings.clear_with_capacity();
        for (auto* stream : m_streams) {
            if (stream->try_ref())
                streams.append(adopt_ref(*stream));
        }
        for (auto* stream : m_record_streams) {
            if (stream->try_ref())
                record_streams.append(adopt_ref(*stream));
        }
        for (auto& [stream_id, ring] : m_capture_rings)
            capture_rings.append({ .stream_id = stream_id, .ring = ring });
        m_pump_woken = false;

        if (streams.is_empty() && record_streams.is_empty()) {
            m_pump_condition.wait();
            continue;
        }

        auto period = m_pump_period;
        locker.unlock();
        for (auto& stream : streams)
            stream->fill(scratch);
        for (auto& [stream_id, ring] : capture_rings) {
            auto frames_available = ring.frames_available();
            if (frames_available == 0)
                continue;
            scratch.resize(frames_available * ring.channel_count());
            auto frames_popped = ring.try_pop(scratch);
            auto samples = scratch.span().trim(frames_popped * ring.channel_count());
            for (auto& stream : record_streams) {
                if (stream->stream_id() == stream_id)
                    stream->deliver(samples);
            }
        }
        // Drop the references before sleeping, so a stream's last reference does not idle here.
        streams.clear_with_capacity();
        record_streams.clear_with_capacity();
        capture_rings.clear_with_capacity();
        locker.lock();

        if (!m_pump_woken && !m_pump_should_exit)
            (void)m_pump_condition.wait_for(period);
    }
}

}
